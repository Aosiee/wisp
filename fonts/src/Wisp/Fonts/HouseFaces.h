#pragma once

// Wisp's house faces, built in: JetBrains Mono Regular, Medium and SemiBold
// (the SIL Open Font License, fonts/ttf/JetBrainsMono-OFL.txt) and
// TinyIcons, the icon font Wisp/Icons.h names. A program that ships no font
// files of its own adds them and has Wisp's look.
//
//     wisp::FontCache fonts;
//     const wisp::fonts::HouseFaces faces = wisp::fonts::AddHouseFaces(fonts);
//     ... wisp::UIApplication::Init with &fonts ...
//     wisp::fonts::ApplyHouseFaces(wisp::UIApplication::Get(), faces);

#include "Wisp/FontCache.h"

#include <cstdint>
#include <span>

namespace wisp {
	class UIApplication;
}

namespace wisp::fonts {

	enum class Face : uint8_t { Regular, Medium, SemiBold, Icons };

	// The face's TTF bytes (static storage).
	std::span<const uint8_t> FaceData(Face face);

	struct HouseFaces
	{
		FontHandle Regular = InvalidFont;
		FontHandle Medium = InvalidFont;
		FontHandle SemiBold = InvalidFont;
		FontHandle Mono = InvalidFont; // JetBrains Mono is monospaced: the Regular face
		FontHandle Icons = InvalidFont;
	};

	// Adds the faces to the cache, each text face falling back to TinyIcons
	// (an icon codepoint in a label draws inline), and rasterizes printable
	// ASCII at `warmSize` so the first frames do not grow the atlas (0 = no
	// warm-up).
	HouseFaces AddHouseFaces(FontCache& cache, float warmSize = 0.0f);

	// The faces as the application's font roles (Regular, Medium, SemiBold,
	// Mono, Icons).
	void ApplyHouseFaces(UIApplication& app, const HouseFaces& faces);

} // namespace wisp::fonts
