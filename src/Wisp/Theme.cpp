#include "Wisp/Theme.h"
#include "Wisp/Core.h"
#include "Wisp/Style.h"
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <fstream>
#include <cstring>

namespace wisp {

static glm::vec4 ParseColour(const rapidjson::Value& v, const glm::vec4& fallback = {1,1,1,1})
{
	if (!v.IsArray() || v.Size() < 3) return fallback;
	glm::vec4 c(fallback);
	c.r = v[0].GetFloat();
	c.g = v[1].GetFloat();
	c.b = v[2].GetFloat();
	if (v.Size() >= 4) c.a = v[3].GetFloat();
	return c;
}

static void LoadButtonStyle(Theme* theme, const rapidjson::Value& obj)
{
	ButtonStyle s;
	if (obj.HasMember("Normal"))   s.Normal.Tint   = ParseColour(obj["Normal"],   s.Normal.Tint);
	if (obj.HasMember("Hovered"))  s.Hovered.Tint  = ParseColour(obj["Hovered"],  s.Hovered.Tint);
	if (obj.HasMember("Pressed"))  s.Pressed.Tint  = ParseColour(obj["Pressed"],  s.Pressed.Tint);
	if (obj.HasMember("Disabled")) s.Disabled.Tint = ParseColour(obj["Disabled"], s.Disabled.Tint);
	theme->SetStyle(s);
}

static void LoadCheckBoxStyle(Theme* theme, const rapidjson::Value& obj)
{
	CheckBoxStyle s;
	if (obj.HasMember("BoxUnchecked")) s.BoxUnchecked.Tint = ParseColour(obj["BoxUnchecked"], s.BoxUnchecked.Tint);
	if (obj.HasMember("BoxHovered"))   s.BoxHovered.Tint   = ParseColour(obj["BoxHovered"],   s.BoxHovered.Tint);
	if (obj.HasMember("BoxChecked"))   s.BoxChecked.Tint   = ParseColour(obj["BoxChecked"],   s.BoxChecked.Tint);
	if (obj.HasMember("CheckMark"))    s.CheckMark.Tint    = ParseColour(obj["CheckMark"],    s.CheckMark.Tint);
	theme->SetStyle(s);
}

static void LoadSliderStyle(Theme* theme, const rapidjson::Value& obj)
{
	SliderStyle s;
	if (obj.HasMember("Track"))        s.Track.Tint        = ParseColour(obj["Track"],        s.Track.Tint);
	if (obj.HasMember("Fill"))         s.Fill.Tint         = ParseColour(obj["Fill"],         s.Fill.Tint);
	if (obj.HasMember("Thumb"))        s.Thumb.Tint        = ParseColour(obj["Thumb"],        s.Thumb.Tint);
	if (obj.HasMember("ThumbHovered")) s.ThumbHovered.Tint = ParseColour(obj["ThumbHovered"], s.ThumbHovered.Tint);
	if (obj.HasMember("ThumbDragged")) s.ThumbDragged.Tint = ParseColour(obj["ThumbDragged"], s.ThumbDragged.Tint);
	theme->SetStyle(s);
}

static void LoadProgressBarStyle(Theme* theme, const rapidjson::Value& obj)
{
	ProgressBarStyle s;
	if (obj.HasMember("Track"))   s.Track.Tint   = ParseColour(obj["Track"],   s.Track.Tint);
	if (obj.HasMember("Fill"))    s.Fill.Tint    = ParseColour(obj["Fill"],    s.Fill.Tint);
	if (obj.HasMember("Marquee")) s.Marquee.Tint = ParseColour(obj["Marquee"], s.Marquee.Tint);
	theme->SetStyle(s);
}

static void LoadEditableTextStyle(Theme* theme, const rapidjson::Value& obj)
{
	EditableTextStyle s;
	if (obj.HasMember("Background"))         s.Background.Tint         = ParseColour(obj["Background"],         s.Background.Tint);
	if (obj.HasMember("HoveredBackground"))  s.HoveredBackground.Tint  = ParseColour(obj["HoveredBackground"],  s.HoveredBackground.Tint);
	if (obj.HasMember("FocusedBackground"))  s.FocusedBackground.Tint  = ParseColour(obj["FocusedBackground"],  s.FocusedBackground.Tint);
	theme->SetStyle(s);
}

static void LoadPanelStyle(Theme* theme, const rapidjson::Value& obj)
{
	PanelStyle s;
	if (obj.HasMember("Background")) s.Background.Tint = ParseColour(obj["Background"], s.Background.Tint);
	theme->SetStyle(s);
}

bool Theme::LoadFromFile(const char* path)
{
	std::ifstream ifs(path);
	if (!ifs.is_open()) return false;

	rapidjson::IStreamWrapper isw(ifs);
	rapidjson::Document doc;
	doc.ParseStream(isw);
	if (doc.HasParseError()) return false;

	if (!doc.IsObject()) return false;

	m_Colours.clear();
	m_Styles.clear();
	m_Version++;

	if (doc.HasMember("colours") && doc["colours"].IsObject())
	{
		for (auto it = doc["colours"].MemberBegin(); it != doc["colours"].MemberEnd(); ++it)
		{
			m_Colours[it->name.GetString()] = ParseColour(it->value);
		}
	}

	if (doc.HasMember("styles") && doc["styles"].IsObject())
	{
		const auto& styles = doc["styles"];
		if (styles.HasMember("ButtonStyle"))       LoadButtonStyle(this, styles["ButtonStyle"]);
		if (styles.HasMember("CheckBoxStyle"))     LoadCheckBoxStyle(this, styles["CheckBoxStyle"]);
		if (styles.HasMember("SliderStyle"))       LoadSliderStyle(this, styles["SliderStyle"]);
		if (styles.HasMember("ProgressBarStyle"))  LoadProgressBarStyle(this, styles["ProgressBarStyle"]);
		if (styles.HasMember("EditableTextStyle")) LoadEditableTextStyle(this, styles["EditableTextStyle"]);
		if (styles.HasMember("PanelStyle"))        LoadPanelStyle(this, styles["PanelStyle"]);
	}

	return true;
}

glm::vec4 ResolveThemeColour(const Widget* widget, const std::string& key, const glm::vec4& fallback)
{
	for (const Widget* w = widget; w; w = w->GetParent())
	{
		if (w->GetTheme())
		{
			glm::vec4 c = w->GetTheme()->GetColour(key, fallback);
			if (c != fallback || w->GetTheme()->GetColour(key, glm::vec4(-1)) != glm::vec4(-1))
				return c;
		}
	}
	return fallback;
}

} // namespace wisp
