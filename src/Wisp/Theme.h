#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>

namespace wisp {

class Widget;

// ---- Type-erased style storage ---------------------------------------------

class IStyleHolder
{
public:
	virtual ~IStyleHolder() = default;
};

template<typename S>
class StyleHolder : public IStyleHolder
{
public:
	explicit StyleHolder(const S& s) : style(s) {}
	S style;
};

// ---- Theme -----------------------------------------------------------------

class Theme
{
public:
	Theme() = default;
	~Theme() = default;

	bool LoadFromFile(const char* path);

	// Colour API (backward compat)
	glm::vec4 GetColour(const std::string& key, const glm::vec4& fallback = {1,1,1,1}) const
	{
		auto it = m_Colours.find(key);
		return it != m_Colours.end() ? it->second : fallback;
	}
	void SetColour(const std::string& key, const glm::vec4& c) { m_Colours[key] = c; }

	// Typed style API
	template<typename S>
	void SetStyle(const S& s)
	{
		m_Styles[S::StyleTypeName] = std::make_unique<StyleHolder<S>>(s);
		m_Version++;
	}

	template<typename S>
	const S* GetStyle() const
	{
		auto it = m_Styles.find(S::StyleTypeName);
		if (it != m_Styles.end())
			return &static_cast<StyleHolder<S>*>(it->second.get())->style;
		return nullptr;
	}

	void SetParentTheme(std::shared_ptr<Theme> p) { m_Parent = std::move(p); m_Version++; }
	std::shared_ptr<Theme> GetParentTheme() const { return m_Parent; }

	uint64_t GetVersion() const { return m_Version; }
	void BumpVersion() { m_Version++; }

private:
	std::unordered_map<std::string, glm::vec4> m_Colours;
	std::unordered_map<std::string, std::unique_ptr<IStyleHolder>> m_Styles;
	std::shared_ptr<Theme> m_Parent;
	uint64_t m_Version = 1;
};

// ---- Resolution ------------------------------------------------------------

// Walks widget→parent chain looking for a Theme that has style S.
// Returns the first found, or the provided fallback.
template<typename S>
S ResolveStyle(const Widget* widget, const S& fallback)
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

// Legacy helper — kept for Border's ResolveThemeColour usage
glm::vec4 ResolveThemeColour(const Widget* widget, const std::string& key, const glm::vec4& fallback);

} // namespace wisp
