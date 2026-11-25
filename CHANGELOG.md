# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the API; every entry says what callers need to change.

## 0.3.0

Applets: Wisp as a program of its own, on Hearth's `hearth::App`.

- Built on Hearth 0.5.0.
- `wisp::applet` (`Wisp/Applet/Applet.h`): `wisp::Applet` starts the
  application with `RhiRenderer`, puts the main window's `RhiFrameClient`
  on the app's presenter, routes the platform's events to Wisp by window,
  and is the dock's `WindowHost` (a torn-out panel gets its own window and
  presenter; none offscreen). `--input-script <json>` plays recorded input
  (move, click, down, up, wheel, key, text by frame) as platform events,
  and a capture waits for its last frame.
- `wisp::fonts` (`Wisp/Fonts/HouseFaces.h`): JetBrains Mono Regular,
  Medium and SemiBold and TinyIcons built in, added with the icon fallback
  (`AddHouseFaces`) and set as the font roles (`ApplyHouseFaces`). The
  arrays are generated from `fonts/ttf/` (`cmake -DMODE=update -P
  fonts/Embed.cmake`); CI checks them.
- `wisp_gallery` (`gallery/`): every base widget, the dock with a panel in
  a second window, the code editor, a graph canvas, the charts and the
  document view. CI captures it offscreen on lavapipe, twice, and compares
  the two.
- The GPU suite draws: an applet's root read back from the offscreen
  image, and the input script clicking a button.

Nothing changes for callers of 0.2. Wisp needs Hearth 0.5.0 (for
`hearth::app`; `-DWISP_APPLET=OFF` builds without the applet and the
gallery).

## 0.2.0

Wisp's GPU renderer: TinyEngine's UI renderer and frame client, as
`wisp::rhi`.

- Built on Hearth 0.3.0.
- `wisp::rhi` (the `WISP_RHI` option, on by default): the UI drawn with
  Hearth's GPU layer.
  - `RhiRenderer`: an `IUIRenderer` that builds its pipelines from the
    built-in shaders and keeps its textures on Hearth's device.
    `WrapImage` shows an image the program owns.
  - `RhiFrameClient`: one of Hearth's `FrameClient`s. It builds a window's
    batches in the update and records them in a "Wisp UI" pass over the
    swapchain. `Draw` records them into a pass of the program's own (an
    offscreen target).
- The UI shaders' GLSL is in `rhi/shaders/`, with its SPIR-V committed
  beside the renderer. `WISP_SHADER_TOOLS` adds `wisp_shaders_check` and
  `wisp_shaders_update`, built with glslang 16.0.0. CI fails when the
  committed SPIR-V does not match the GLSL.
- The suite gains `wisp_rhi_gpu_tests` (run on Mesa's lavapipe in CI).

Nothing changes for callers of 0.1: `wisp::rhi` is new, and `wisp::wisp`
still names no GPU API.

## 0.1.1

- Built on Hearth 0.1.1: Hearth 0.1.0 names a GLFW commit that can no
  longer be fetched, so Wisp 0.1.0 does not build from source.

Nothing changes for callers.

## 0.1.0

The first release: TinyEngine's UI library as a library of its own, moved
out with its history.

- Built on Hearth 0.1.0 and tmath 0.1.0.
- `wisp::wisp` on Hearth's platform and host seam and on tmath; nothing
  else of the program's is linked.
- `UIApplication::Init(UIApplicationDesc)`: the fonts, the platform, the
  renderer and the render-idle wait are the program's. `UpdateWindow`
  builds a window's frame (into an optional `FrameArena`) and
  `GetFrameBatches` hands its batches to the program's record.
- `IUIRenderer` (`Wisp/Renderer.h`): textures and the per-window
  `WindowRenderState`; the renderer is the program's.
- `WindowHost` (`Wisp/Docking/WindowHost.h`): the OS windows a torn-out
  dock window needs. Without one, torn-out panels stay in the layout.
- `wisp::text`: the Markdown parser and the Luau tokenizer, with no UI.
- The suite on hamr's test library (`wisp_tests`, `wisp_text_tests`).
