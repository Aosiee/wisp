# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the API; every entry says what callers need to change.

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
