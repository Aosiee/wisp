# Wisp - a retained-mode UI library: widgets, layout, docking, text, code
# and document views, drawn as batches a renderer records. This file is the
# standalone build and what a host's CMake build adds with add_subdirectory.
cmake_minimum_required(VERSION 3.20)

# The version is set once, in the header.
file(STRINGS src/Wisp/Version.h WISP_VERSION_LINE REGEX "^#define WISP_VERSION_STRING ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" WISP_VERSION_FROM_HEADER "${WISP_VERSION_LINE}")
project(wisp VERSION ${WISP_VERSION_FROM_HEADER} LANGUAGES C CXX)

if(CMAKE_VERSION VERSION_LESS 3.21)
    string(COMPARE EQUAL "${CMAKE_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" PROJECT_IS_TOP_LEVEL)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

option(WISP_BUILD_TESTS "Build the suite" ${PROJECT_IS_TOP_LEVEL})
option(WISP_DEBUG_TOOLS "Build the widget reflector and the focus overlay" ON)
option(WISP_RHI "Build wisp::rhi, the renderer and frame client on Hearth's GPU layer" ON)
# The UI shaders' SPIR-V is committed (rhi/src/Wisp/Rhi/Shaders/); this
# builds the pinned glslang to check it against the GLSL (wisp_shaders_check)
# or rewrite it (wisp_shaders_update).
option(WISP_SHADER_TOOLS "Build glslang for the shader check" OFF)
# address | undefined: the sanitizer the library and the suite build with.
set(WISP_SANITIZE "" CACHE STRING "Build with a sanitizer (address or undefined)")
# Checkouts to use instead of fetching the pinned commits below.
set(WISP_HEARTH_DIR "" CACHE PATH "A Hearth checkout (empty: fetch the pinned commit)")
set(WISP_TMATH_DIR "" CACHE PATH "A tmath checkout (empty: fetch the pinned commit)")
set(WISP_RAPIDJSON_DIR "" CACHE PATH "A RapidJSON checkout (empty: fetch the pinned commit)")
set(WISP_STB_DIR "" CACHE PATH "An stb checkout (empty: fetch the pinned commit)")
set(WISP_HAMR_DIR "" CACHE PATH "A hamr checkout, for the suite's test library (empty: fetch the pinned commit)")
set(WISP_GLSLANG_DIR "" CACHE PATH "A glslang checkout, for the shader tools (empty: fetch the pinned tag)")

# The pinned dependencies: Hearth (the host seam, the platform and, for
# wisp::rhi, the GPU layer and its window presenter), tmath
# (and glm through it), RapidJSON (the dock layout's files), stb_truetype
# (the glyph rasterizer) and hamr's C++ test library for the suite.
set(WISP_HEARTH_COMMIT 3ecc667823ba792343b4a803ff0b99177c4abac3) # Hearth 0.3.0 (v0.3.0)
set(WISP_TMATH_COMMIT 20226ccfd033d66985a720da5028b5887c74ee74) # tmath 0.1.0 (v0.1.0)
set(WISP_RAPIDJSON_COMMIT 24b5e7a8b27f42fa16b96fc70aade9106cf7102f)
set(WISP_STB_COMMIT 6b5799bae8a1fe3792691ef4b72ae26434c3e799)
set(WISP_HAMR_COMMIT a4d43e9ffe9c084955d28aab5d77c71ff2c8f597) # hamr 0.1.0 (v0.1.0)
set(WISP_GLSLANG_TAG 16.0.0) # the shader tools only

# A library built with its own CMake (Hearth, tmath), unless the host
# already has its target.
function(wisp_project_of name url commit dir_var)
    if(${dir_var})
        add_subdirectory(${${dir_var}} ${CMAKE_BINARY_DIR}/${name} EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(${name} GIT_REPOSITORY ${url} GIT_TAG ${commit})
        FetchContent_MakeAvailable(${name})
    endif()
endfunction()

# Only headers are needed (RapidJSON, stb, hamr), so the project's own
# CMake build does not run: SOURCE_SUBDIR names a directory without a
# CMakeLists.txt.
function(wisp_source_of name url commit dir_var out_var)
    if(${dir_var})
        set(${out_var} "${${dir_var}}" PARENT_SCOPE)
        return()
    endif()
    FetchContent_Declare(${name} GIT_REPOSITORY ${url} GIT_TAG ${commit} SOURCE_SUBDIR wisp-headers-only)
    FetchContent_MakeAvailable(${name})
    set(${out_var} "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

if(NOT TARGET tmath::tmath)
    wisp_project_of(tmath https://github.com/Aosiee/tmath.git ${WISP_TMATH_COMMIT} WISP_TMATH_DIR)
endif()
if(NOT TARGET hearth::platform)
    wisp_project_of(hearth https://github.com/Aosiee/hearth.git ${WISP_HEARTH_COMMIT} WISP_HEARTH_DIR)
endif()
wisp_source_of(rapidjson https://github.com/Tencent/rapidjson.git ${WISP_RAPIDJSON_COMMIT} WISP_RAPIDJSON_DIR WISP_RAPIDJSON_SOURCE)
wisp_source_of(stb https://github.com/nothings/stb.git ${WISP_STB_COMMIT} WISP_STB_DIR WISP_STB_SOURCE)

set(WISP_SANITIZE_FLAGS "")
if(WISP_SANITIZE)
    set(WISP_SANITIZE_FLAGS -fsanitize=${WISP_SANITIZE} -fno-omit-frame-pointer)
endif()

function(wisp_flags target)
    if(PROJECT_IS_TOP_LEVEL AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WISP_SANITIZE_FLAGS)
        target_compile_options(${target} PRIVATE ${WISP_SANITIZE_FLAGS})
        target_link_options(${target} PRIVATE ${WISP_SANITIZE_FLAGS})
    endif()
endfunction()

# wisp::text - the text models with no UI: the Markdown parser (Wisp/Doc/)
# and the Luau tokenizer (Wisp/Code/), for a tool that renders documents or
# code without Wisp's widgets.
file(GLOB_RECURSE WISP_TEXT_SOURCES CONFIGURE_DEPENDS text/src/*.cpp)
add_library(wisp_text STATIC ${WISP_TEXT_SOURCES})
add_library(wisp::text ALIAS wisp_text)
target_include_directories(wisp_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/text/src)
target_compile_features(wisp_text PUBLIC cxx_std_20)
wisp_flags(wisp_text)

# wisp::wisp - the UI. The program supplies the renderer (Wisp/Renderer.h),
# the OS windows a torn-out dock window needs (Wisp/Docking/WindowHost.h)
# and Hearth's host functions.
file(GLOB_RECURSE WISP_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(wisp STATIC ${WISP_SOURCES})
add_library(wisp::wisp ALIAS wisp)
target_include_directories(wisp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(wisp SYSTEM PUBLIC "${WISP_RAPIDJSON_SOURCE}/include" PRIVATE "${WISP_STB_SOURCE}")
target_link_libraries(wisp PUBLIC wisp_text tmath::tmath hearth::core hearth::platform)
target_precompile_headers(wisp PRIVATE src/Wisp/Pch.h)
if(WISP_DEBUG_TOOLS)
    target_compile_definitions(wisp PUBLIC WISP_DEBUG_TOOLS)
endif()
wisp_flags(wisp)

# wisp::rhi - Wisp drawn through Hearth's GPU layer: RhiRenderer (the UI
# pipelines, the atlas, the textures, each window's staging) and
# RhiFrameClient (a window's UI as one of its presenter's frame clients).
if(WISP_RHI)
    file(GLOB_RECURSE WISP_RHI_SOURCES CONFIGURE_DEPENDS rhi/src/*.cpp)
    add_library(wisp_rhi STATIC ${WISP_RHI_SOURCES})
    add_library(wisp::rhi ALIAS wisp_rhi)
    target_include_directories(wisp_rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rhi/src)
    target_link_libraries(wisp_rhi PUBLIC wisp hearth::rhi)
    wisp_flags(wisp_rhi)
endif()

if(WISP_SHADER_TOOLS)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    if(WISP_GLSLANG_DIR)
        add_subdirectory(${WISP_GLSLANG_DIR} ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG ${WISP_GLSLANG_TAG})
        FetchContent_MakeAvailable(glslang)
    endif()

    set(WISP_SHADER_NAMES ui_image.frag ui_image.vert ui_rect.frag ui_rect.vert
        ui_roundedbox.frag ui_roundedbox.vert ui_text.frag ui_text.vert)
    set(WISP_SHADER_OUTPUTS "")
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/wisp-shaders)
    foreach(shader ${WISP_SHADER_NAMES})
        string(REPLACE "." "_" variable ${shader})
        set(output ${CMAKE_BINARY_DIR}/wisp-shaders/${shader}.h)
        add_custom_command(OUTPUT ${output}
            COMMAND glslang-standalone -V --target-env vulkan1.3 --vn ${variable} -o ${output}
                ${CMAKE_CURRENT_SOURCE_DIR}/rhi/shaders/${shader}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rhi/shaders/${shader} glslang-standalone
            COMMENT "glslang ${shader}")
        list(APPEND WISP_SHADER_OUTPUTS ${output})
    endforeach()
    add_custom_target(wisp_shaders_check
        COMMAND ${CMAKE_COMMAND} -DGENERATED=${CMAKE_BINARY_DIR}/wisp-shaders
            -DCOMMITTED=${CMAKE_CURRENT_SOURCE_DIR}/rhi/src/Wisp/Rhi/Shaders -DMODE=check
            -P ${CMAKE_CURRENT_SOURCE_DIR}/rhi/shaders/Sync.cmake
        DEPENDS ${WISP_SHADER_OUTPUTS})
    add_custom_target(wisp_shaders_update
        COMMAND ${CMAKE_COMMAND} -DGENERATED=${CMAKE_BINARY_DIR}/wisp-shaders
            -DCOMMITTED=${CMAKE_CURRENT_SOURCE_DIR}/rhi/src/Wisp/Rhi/Shaders -DMODE=update
            -P ${CMAKE_CURRENT_SOURCE_DIR}/rhi/shaders/Sync.cmake
        DEPENDS ${WISP_SHADER_OUTPUTS})
endif()

if(WISP_BUILD_TESTS)
    wisp_source_of(hamr https://github.com/Aosiee/hamr.git ${WISP_HAMR_COMMIT} WISP_HAMR_DIR WISP_HAMR_SOURCE)

    add_executable(wisp_text_tests tests/main.cpp tests/MarkdownTests.cpp)
    target_include_directories(wisp_text_tests PRIVATE "${WISP_HAMR_SOURCE}/include")
    target_link_libraries(wisp_text_tests PRIVATE wisp_text)
    wisp_flags(wisp_text_tests)

    add_executable(wisp_tests tests/main.cpp tests/CodeModelTests.cpp tests/ContrastTests.cpp tests/DockModelTests.cpp
        tests/FontCacheTests.cpp tests/GraphTests.cpp)
    target_include_directories(wisp_tests PRIVATE "${WISP_HAMR_SOURCE}/include")
    target_link_libraries(wisp_tests PRIVATE wisp hearth::default_host)
    wisp_flags(wisp_tests)

    # wisp::rhi on a device (lavapipe in CI).
    if(WISP_RHI)
        add_executable(wisp_rhi_gpu_tests tests/main.cpp tests/RhiGpuTests.cpp)
        target_include_directories(wisp_rhi_gpu_tests PRIVATE "${WISP_HAMR_SOURCE}/include")
        target_link_libraries(wisp_rhi_gpu_tests PRIVATE wisp_rhi hearth::default_host)
        wisp_flags(wisp_rhi_gpu_tests)
    endif()
endif()
