#include "Wisp/Fonts/HouseFaces.h"

#include "Wisp/Application.h"

#include <vector>

namespace wisp::fonts {

	namespace {

		constexpr uint8_t kRegular[] = {
#include "Wisp/Fonts/Embedded/JetBrainsMono-Regular.inc"
		};
		constexpr uint8_t kMedium[] = {
#include "Wisp/Fonts/Embedded/JetBrainsMono-Medium.inc"
		};
		constexpr uint8_t kSemiBold[] = {
#include "Wisp/Fonts/Embedded/JetBrainsMono-SemiBold.inc"
		};
		constexpr uint8_t kIcons[] = {
#include "Wisp/Fonts/Embedded/TinyIcons.inc"
		};

		FontHandle Add(FontCache& cache, Face face, const char* name)
		{
			const std::span<const uint8_t> data = FaceData(face);
			// The name is the face's identity for a reload (none is built in).
			return cache.AddFont(data.data(), data.size(), name);
		}

	} // namespace

	std::span<const uint8_t> FaceData(Face face)
	{
		switch (face)
		{
		case Face::Regular: return kRegular;
		case Face::Medium: return kMedium;
		case Face::SemiBold: return kSemiBold;
		case Face::Icons: return kIcons;
		}
		return {};
	}

	HouseFaces AddHouseFaces(FontCache& cache, float warmSize)
	{
		HouseFaces faces;
		faces.Regular = Add(cache, Face::Regular, "wisp:JetBrainsMono-Regular");
		faces.Medium = Add(cache, Face::Medium, "wisp:JetBrainsMono-Medium");
		faces.SemiBold = Add(cache, Face::SemiBold, "wisp:JetBrainsMono-SemiBold");
		faces.Mono = faces.Regular;
		faces.Icons = Add(cache, Face::Icons, "wisp:TinyIcons");
		const std::vector<FontHandle> fallbacks = { faces.Icons };
		for (const FontHandle text : { faces.Regular, faces.Medium, faces.SemiBold })
			cache.SetFallbacks(text, fallbacks);
		if (warmSize > 0.0f)
		{
			for (uint32_t c = 32; c < 127; ++c)
			{
				cache.GetGlyph(faces.Regular, c, warmSize);
				cache.GetGlyph(faces.SemiBold, c, warmSize);
			}
		}
		return faces;
	}

	void ApplyHouseFaces(UIApplication& app, const HouseFaces& faces)
	{
		app.SetFont(FontRole::Regular, faces.Regular);
		app.SetFont(FontRole::Medium, faces.Medium);
		app.SetFont(FontRole::SemiBold, faces.SemiBold);
		app.SetFont(FontRole::Mono, faces.Mono);
		app.SetFont(FontRole::Icons, faces.Icons);
	}

} // namespace wisp::fonts
