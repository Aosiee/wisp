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

// Pulled in before GLFW gets included anywhere in a translation unit so GLFW
// notices Vulkan's types are already visible and declares its own Vulkan helper
// functions (glfwCreateWindowSurface, glfwGetRequiredInstanceExtensions, etc).
//
// We include the umbrella <vulkan/vulkan.h> ourselves first (with
// VK_NO_PROTOTYPES so it declares types only, no function symbols to link
// against) rather than let volk.h pull in just vulkan_core.h on its own.
//
// Volk is our vendored Vulkan meta-loader (third_party plugin), it loads
// every Vulkan function dynamically at runtime instead of linking against the
// Vulkan loader, see Platform/Vulkan/VulkanCommon.h.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>

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