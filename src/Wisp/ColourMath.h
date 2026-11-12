#pragma once

#include "Math/Colour.h"

#include <glm/glm.hpp>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace wisp {

// ---- HSV conversion --------------------------------------------------------
// the HSV <-> RGB conversions live in Math/Colour.h; the CMYK and hex
// conversions below are the picker's own.

using TinyEngine::Math::RgbToHsv;
using TinyEngine::Math::HsvToRgb;

// ---- CMYK (0..1 per component) ---------------------------------------------

inline glm::vec4 RgbToCmyk(const glm::vec3& rgb)
{
	float k = 1.0f - std::max(rgb.r, std::max(rgb.g, rgb.b));
	if (k >= 1.0f - 1e-6f) return { 0.0f, 0.0f, 0.0f, 1.0f };
	float inv = 1.0f / (1.0f - k);
	return {
		(1.0f - rgb.r - k) * inv,
		(1.0f - rgb.g - k) * inv,
		(1.0f - rgb.b - k) * inv,
		k
	};
}

inline glm::vec3 CmykToRgb(const glm::vec4& cmyk)
{
	float c = glm::clamp(cmyk.x, 0.0f, 1.0f);
	float m = glm::clamp(cmyk.y, 0.0f, 1.0f);
	float y = glm::clamp(cmyk.z, 0.0f, 1.0f);
	float k = glm::clamp(cmyk.w, 0.0f, 1.0f);
	return {
		(1.0f - c) * (1.0f - k),
		(1.0f - m) * (1.0f - k),
		(1.0f - y) * (1.0f - k)
	};
}

// ---- Hex strings ------------------------------------------------------------

// 6-digit #RRGGBB (alpha is handled by its own bar).
inline std::string RgbaToHex(const glm::vec4& rgba)
{
	auto byte = [](float f) {
		int v = (int)std::lround(glm::clamp(f, 0.0f, 1.0f) * 255.0f);
		return v;
	};
	char buf[8];
	std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(rgba.r), byte(rgba.g), byte(rgba.b));
	return buf;
}

// Accepts #RRGGBB or #RRGGBBAA (leading '#' optional). On success writes out
// (alpha only touched by the 8-digit form) and returns true.
inline bool HexToRgba(const std::string& text, glm::vec4& out)
{
	const char* s = text.c_str();
	if (*s == '#') s++;
	int len = (int)std::strlen(s);
	if (len != 6 && len != 8) return false;
	for (int i = 0; i < len; i++)
		if (!std::isxdigit((unsigned char)s[i])) return false;

	auto byte = [&](int idx) -> float {
		unsigned int v = 0;
		char pair[3] = { s[idx * 2], s[idx * 2 + 1], '\0' };
		std::sscanf(pair, "%2x", &v);
		return (float)v / 255.0f;
	};
	out.r = byte(0);
	out.g = byte(1);
	out.b = byte(2);
	if (len == 8) out.a = byte(3);
	return true;
}

} // namespace wisp
