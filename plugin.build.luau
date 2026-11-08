-- Wisp: the engine's retained-mode UI framework (Slate-style). Split out of
-- the engine plugin's sources into its own
-- engine-layer plugin — the engine plugin contains zero Wisp header includes,
-- only a std::function draw-callback slot (Platform/Vulkan) that the Wisp
-- renderer registers into.
--
-- Sources/headers live IN the plugin (spec §0): src/Wisp/ — the tree keeps
-- the same interleaved layout, and the include root is src/, so every
-- existing "Wisp/..." include (intra-Wisp and consumer sites in the editor
-- and game plugins) resolves unchanged. The pch is a byte-identical copy of
-- the engine's tepch.h, so each moved TU compiles exactly as it did inside
-- the engine module.
--
-- Flavor pair, mirroring engine/engine_editor: the SAME sources compile once
-- per target under two module names (exactly one has sources per build, so
-- nothing is double-compiled). Wisp has no TE_EDITOR branches, but the
-- module-name flavoring is still REQUIRED — Kiln's compile_pch output dir is
-- keyed by module name, not by config tuple, and a gch is a
-- non-deterministic compiler-state dump (outputs_opaque): a single module
-- name compiled by both the editor and sandbox flavors would share one gch
-- path and re-verify-mismatch forever. Distinct module names give each
-- flavor its own pch/obj dirs. (A latent Kiln lowering issue.)

plugin "wisp" {
    enabled_by_default = false,
    public_deps = {
        "engine", "debug_draw",
        "glfw", "glm", "rapidjson", "stb", "stb_image", "vulkan_headers", "volk",
    },
}

local is_linux = config.platform == "linux"
local is_editor_build = config.features.editor == "1"

-- The UI shaders are committed GLSL cooked by the normal shader
-- rule (the tinycook tool is a build output of the tinycook target).
local wisp_content = "content"
local wisp_tinycook = ".kiln/out/" .. config.platform .. "-" .. config.config .. "/bin/tinycook"

cook "wisp_shaders_vert" {
    rule = "cook_tinyasset",
    inputs = glob(wisp_content .. "/shaders/vulkan/*.vert"),
    params = {
        tool = wisp_tinycook,
        asset_type = "shader",
        stage = "vert",
        lint_strict = (config.config == "release" or config.config == "shipping") and "1" or "0",
        debug_info = (config.config == "shipping") and "0" or "1",
    },
}

cook "wisp_shaders_frag" {
    rule = "cook_tinyasset",
    inputs = glob(wisp_content .. "/shaders/vulkan/*.frag"),
    params = {
        tool = wisp_tinycook,
        asset_type = "shader",
        stage = "frag",
        lint_strict = (config.config == "release" or config.config == "shipping") and "1" or "0",
        debug_info = (config.config == "shipping") and "0" or "1",
    },
}
local sanitizers = require "build/sanitizers"
local conf_define = sanitizers.conf_define
local conf_shipping_define = sanitizers.conf_shipping_define

local wisp_sources = {
    glob("src/**/*.cpp"),
}

local wisp_defines = {
    "GLFW_INCLUDE_NONE",
    "_CRT_SECURE_NO_WARNINGS",
    conf_define,
    is_linux and "TE_PLATFORM_LINUX" or "TE_PLATFORM_WINDOWS",
    conf_shipping_define,
}

local wisp_deps = {
    public = {
        "engine/engine", "debug_draw/debug_draw",
        "glfw/glfw", "glm/glm", "rapidjson/rapidjson", "stb/stb",
        "vulkan_headers/vulkan_headers", "volk/volk",
    },
}

-- The TE_TOOL flavor (a Wisp tool app on the engine_tool subset - see the
-- engine plugin's flavor notes).
local is_tool_build = config.features.tool == "1"

module "wisp" {
    sources = (is_editor_build or is_tool_build) and {} or wisp_sources,
    pch = "src/tepch.h",
    public_includes = { "src" },
    private_defines = wisp_defines,
    deps = wisp_deps,
    flags = sanitizers.flags,
    public_link_flags = sanitizers.link_flags,
    cpp_std = "c++20",
}

module "wisp_editor" {
    sources = is_editor_build and wisp_sources or {},
    pch = "src/tepch.h",
    public_includes = { "src" },
    private_defines = {
        "TE_EDITOR",
        "GLFW_INCLUDE_NONE",
        "_CRT_SECURE_NO_WARNINGS",
        conf_define,
        is_linux and "TE_PLATFORM_LINUX" or "TE_PLATFORM_WINDOWS",
        conf_shipping_define,
    },
    deps = wisp_deps,
    flags = sanitizers.flags,
    public_link_flags = sanitizers.link_flags,
    cpp_std = "c++20",
}

module "wisp_tool" {
    sources = is_tool_build and wisp_sources or {},
    pch = "src/tepch.h",
    public_includes = { "src" },
    private_defines = {
        "TE_TOOL",
        "GLFW_INCLUDE_NONE",
        "_CRT_SECURE_NO_WARNINGS",
        conf_define,
        is_linux and "TE_PLATFORM_LINUX" or "TE_PLATFORM_WINDOWS",
        conf_shipping_define,
    },
    deps = {
        public = {
            "engine/engine_tool", "debug_draw/debug_draw",
            "glfw/glfw", "glm/glm", "rapidjson/rapidjson", "stb/stb",
            "vulkan_headers/vulkan_headers", "volk/volk",
        },
    },
    flags = sanitizers.flags,
    public_link_flags = sanitizers.link_flags,
    cpp_std = "c++20",
}
