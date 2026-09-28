#pragma once

#include <string>

namespace wisp {

// Cross-platform clipboard access via GLFW.
// Works on X11, Wayland, and Win32.

class Clipboard
{
public:
	static std::string Get();
	static void Set(const std::string& text);
};

} // namespace wisp
