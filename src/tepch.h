#pragma once

#include <iostream>
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>

#include <cstdint>
#include <cstdlib>

#include <string>
#include <sstream>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "TinyEngine/Core/Log.h"
#include "TinyEngine/Debug/Profiler.h"

// P-D: Wisp no longer touches the backend directly (the renderer goes
// through the RHI; window/surface creation lives in the engine's window
// layer) - the old Vulkan/volk umbrella include is gone, which also
// satisfies the P-E "no backend identifiers outside Platform/Vulkan" rule.

#ifdef TE_PLATFORM_LINUX
	#include <csignal>              // Signal handling (SIGSEGV, SIGABRT) for crash logging
	#include <GLFW/glfw3.h>         // Must come before glfw3native.h to define GLFWAPI
	#define GLFW_EXPOSE_NATIVE_X11  // Unlocks glfwGetX11Display() and glfwGetX11Window()
	#define GLFW_EXPOSE_NATIVE_WAYLAND // Unlocks glfwGetWaylandDisplay() and glfwGetWaylandWindow()
	#include <GLFW/glfw3native.h>   // Pulls in X11/Xlib.h, Xutil.h, Xrandr.h transitively
	// Undef X11 macros AFTER all X11 headers are done — they pollute the global namespace
	#undef None                     // X11: 0          — conflicts with EventType::None, ShaderDataType::None
	#undef Bool                     // X11: int         — conflicts with ShaderDataType::Bool
	#undef Int                      // X11: int         — conflicts with ShaderDataType::Int
	#undef Status                   // X11: int         — conflicts with common Status types
	#undef True                     // X11: 1           — conflicts with boolean literals/enums
	#undef False                    // X11: 0           — conflicts with boolean literals/enums
	#undef Always                   // X11: GXcopy etc  — conflicts with common enum names
	#undef Success                  // X11: 0           — conflicts with result/error enums
	#undef Unsorted                 // X11: 0           — conflicts with sort enums
	#undef Complex                  // X11: 0           — conflicts with common type names
#endif