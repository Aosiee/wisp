#pragma once

// The UI shaders' SPIR-V, built from rhi/shaders/ by the pinned glslang
// (`-V --target-env vulkan1.3`); CI checks them against the GLSL
// (wisp_shaders_check), and wisp_shaders_update rewrites them.

#include <cstdint>

namespace wisp::shaders {

#include "Wisp/Rhi/Shaders/ui_image.frag.h"
#include "Wisp/Rhi/Shaders/ui_image.vert.h"
#include "Wisp/Rhi/Shaders/ui_rect.frag.h"
#include "Wisp/Rhi/Shaders/ui_rect.vert.h"
#include "Wisp/Rhi/Shaders/ui_roundedbox.frag.h"
#include "Wisp/Rhi/Shaders/ui_roundedbox.vert.h"
#include "Wisp/Rhi/Shaders/ui_text.frag.h"
#include "Wisp/Rhi/Shaders/ui_text.vert.h"

} // namespace wisp::shaders
