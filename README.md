# Wisp

[![Linux](https://github.com/Aosiee/wisp/actions/workflows/linux.yml/badge.svg?branch=dev)](https://github.com/Aosiee/wisp/actions/workflows/linux.yml)
[![Windows: not supported](https://img.shields.io/badge/Windows-not%20supported-lightgrey)](#building)
[![macOS: not supported](https://img.shields.io/badge/macOS-not%20supported-lightgrey)](#building)

[![Release](https://img.shields.io/github/v/release/Aosiee/wisp)](https://github.com/Aosiee/wisp/releases/latest)

A retained-mode UI library for real-time applications and the tools around
them: widgets, layout, popups and menus, drag and drop, animation, a
docking system with torn-out windows, a code editor and a document viewer.
Wisp builds each window's frame into draw batches; the program's renderer
records them.

Wisp stands on [Hearth](https://github.com/Aosiee/hearth) (the platform,
the host seam and, for `wisp::rhi`, the GPU layer) and [tmath](https://github.com/Aosiee/tmath). It was written
for [TinyEngine](https://github.com/Aosiee/TinyEngine)'s editor and tools
and knows nothing about it.

## The libraries

- **`wisp::wisp`** - the UI: `UIApplication`, the widgets (`Wisp/Widgets.h`,
  `Wisp/Components/`, `Wisp/Composites/`), the declarative builder
  (`Wisp/Declarative.h`), docking (`Wisp/Docking/`), the graph view
  (`Wisp/Graph/`), the code editor's model and widget (`Wisp/Code/`,
  `Wisp/Components/CodeEditor.h`), the font cache and the style tokens.
- **`wisp::text`** - the text models with no UI: the Markdown parser
  (`Wisp/Doc/Markdown.h`) and the Luau tokenizer (`Wisp/Code/`), for a tool
  that renders documents or highlights code without Wisp's widgets.
- **`wisp::rhi`** - Wisp drawn through Hearth's GPU layer: `RhiRenderer`
  (`Wisp/Rhi/RhiRenderer.h`, the UI pipelines with their SPIR-V built in,
  the glyph atlas, images and each window's staging) and `RhiFrameClient`
  (`Wisp/Rhi/RhiFrameClient.h`, a window's UI as one of its
  `hearth::rhi::WindowPresenter`'s frame clients: the update, then a pass
  over the swapchain). `-DWISP_RHI=OFF` leaves it out.
- **`wisp::fonts`** - the house faces built in: JetBrains Mono (Regular,
  Medium, SemiBold) and TinyIcons, the icon font `Wisp/Icons.h` names
  (`Wisp/Fonts/HouseFaces.h`: `AddHouseFaces`, `ApplyHouseFaces`).
- **`wisp::im`** - the immediate-mode layer (`Wisp/Im/Immediate.h`): code
  that describes its UI every frame and reads the results back, on the
  retained widgets (below).
- **`wisp::applet`** - Wisp as a program of its own, on
  [Hearth](https://github.com/Aosiee/hearth)'s `hearth::App`
  (`Wisp/Applet/Applet.h`): the renderer, the main window's frame client,
  the platform's events routed to Wisp, the dock's torn-out windows, and an
  input script for scripted captures. `wisp_gallery` (`gallery/`) is the
  sample: every base widget, the dock with a panel in a second window, the
  code editor, a graph canvas, the charts and the document view.

## An applet

The quickest Wisp program is an applet: Hearth opens the window (or draws
offscreen), Wisp fills it.

```cpp
#include "Wisp/Applet/Applet.h"

int main(int argc, char** argv)
{
    hearth::App app(hearth::App::ParseArgs(argc, argv, { .Title = "Hello", .Width = 640, .Height = 400 }));
    wisp::Applet ui(app);
    ui.AddHouseFaces();
    ui.SetRoot(BuildMyUi());
    return app.Run();
}
```

Every applet takes Hearth's flags; offscreen it captures itself:

```sh
wisp_gallery --display-mode offscreen --render-target 1280x800 --capture-ui gallery.png
```

## Immediate mode

For a quick tool, describe the UI every frame instead of building it:

```cpp
#include "Wisp/Im/Immediate.h"

ui.SetRoot(std::make_unique<wisp::ImmediateHost>(&ui.Fonts(), ui.GetHouseFaces().Regular, [&](wisp::im::Context& im) {
    im.Text("Frame {:.1f} ms", frameMs);
    im.Slider("Exposure", &exposure, -4.0f, 4.0f);
    if (im.Button("Reset"))
        exposure = 0.0f;
}));
```

An `ImmediateHost` is an ordinary widget, so an immediate scope sits
anywhere in a retained layout, and `im.Widget<T>(id, factory)` places any
retained widget inside one. Its function runs once a frame, at the start of
its window's update (after the frame's events, before layout). Each call
is matched by its id to the widget the host made for it the first time;
later frames update that widget, and a widget no call reached is destroyed.
So the widgets are Wisp's own, with their look, focus and keyboard rules.

- **Ids:** a call's label is its id within its scope (`"Save##toolbar"`
  shows `Save`); `PushId`/`PopId` keep a loop's items apart. Text,
  separators, spacing, progress bars, images and table rows key on their
  position, so a text that changes every frame keeps its widget.
- **Results** are what the widget saw since the last frame: `Button`
  returns true once per click; a value call writes the edit through its
  pointer and returns true; a value the program changed is shown.
- **Layout:** `Next()` sets the next item's slot (padding, fill,
  alignment); `BeginRow`/`BeginColumn`, `BeginScroll`, `BeginField(label)`
  (a key column and the field's items), `Begin(title)` (a collapsible
  section), `TreeNode`, `BeginTable(columns)`/`TableNextRow`, `Indent`,
  `Separator`, `Spacing`.
- **The calls:** `Text`, `TextColoured`, `Heading`, `KeyValue`, `Button`,
  `Checkbox`, `Radio`, `Slider` (float, int), `DragFloat`, `DragInt`,
  `InputText`, `Dropdown`, `ColourEdit`, `ProgressBar`, `Plot`, `Image`.

`wisp_gallery --widgets immediate` builds its Widgets panel this way; CI
checks it captures the same image as the retained panel.

## What the program supplies

Wisp draws nothing and opens no window by itself. The program gives it:

- **A renderer** (`Wisp/Renderer.h`): `IUIRenderer` creates the textures
  (the glyph atlas, images) and prepares each window's batches; the
  program records them in its own frame. On Hearth's GPU layer that is
  `wisp::rhi`'s `RhiRenderer`, and `RhiFrameClient` does the per-window
  frame work.
- **Windows**: each OS window the program shows Wisp in is a window
  context (`CreateWindowContext`); its input goes to the `On*` calls. A
  dock that tears a panel out asks the program's `WindowHost`
  (`Wisp/Docking/WindowHost.h`) for the window; without one, torn-out
  panels stay in the layout.
- **Hearth's host functions** (logging, asserts, profiler zones), or
  `hearth::default_host`.

```cpp
#include "Wisp/Application.h"
#include "Wisp/Declarative.h"

wisp::UIApplicationDesc desc;
desc.Fonts = &fonts;                  // a wisp::FontCache with a face added
desc.Platform = platform.get();       // a hearth::platform::Platform
desc.Renderer = std::make_unique<MyRenderer>();
wisp::UIApplication::Init(std::move(desc));

auto& app = wisp::UIApplication::Get();
app.CreateWindowContext(&window, BuildRoot());

// Each frame, for each window:
app.UpdateWindow(&window, width, height, frameIndex, /*arena*/ nullptr);
wisp::WindowRenderState* state = nullptr;
if (const wisp::BatchedDrawData* batches = app.GetFrameBatches(&window, frameIndex, &state))
    myRenderer.Record(*batches, *state);
```

## Building

C++20, Hearth, tmath (and glm through it), RapidJSON and stb_truetype,
each fetched at the commit `CMakeLists.txt` pins (`-DWISP_<NAME>_DIR=<dir>`
uses a local checkout instead), and for the suite hamr's C++ test library.
Linux is what is checked today; `-DHEARTH_NULL_PLATFORM_ONLY=ON` builds
Hearth's GLFW with its null platform alone (no window-system libraries, as
CI does).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHEARTH_NULL_PLATFORM_ONLY=ON
cmake --build build
build/wisp_tests && build/wisp_text_tests
build/wisp_rhi_gpu_tests   # needs a Vulkan device
```

The UI shaders' GLSL is in `rhi/shaders/` and their SPIR-V is committed
beside the renderer (`rhi/src/Wisp/Rhi/Shaders/`). After a shader edit,
configure with `-DWISP_SHADER_TOOLS=ON` (it builds the pinned glslang;
`-DWISP_GLSLANG_DIR=<dir>` uses a local checkout) and build
`wisp_shaders_update`; CI builds `wisp_shaders_check`.

A host build adds Wisp with `add_subdirectory` and links `wisp::wisp` (or
`wisp::text` alone); a host that already has Hearth or tmath targets keeps
its own. `-DWISP_DEBUG_TOOLS=OFF` leaves out the widget reflector and the
focus overlay. The full suite, side by side and in every configuration,
runs through [hamr](https://github.com/Aosiee/hamr): `hamr run`
(`--tier nightly` adds the sanitizers).

## Branches and releases

`main` holds releases only, each one tagged. New work is built on a stream
branched from `dev`, lands on `dev` once it passes locally, and is checked
there by CI; when enough has gathered, `dev` becomes the next release. See
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

The Wisp License: MIT-style, with one condition: a product that includes
Wisp credits it in its credits or about box. See [LICENSE](LICENSE).
TinyIcons is Wisp's, under the same license. JetBrains Mono, built into
`wisp::fonts`, and the suite's test face, Adwaita Mono, are under the SIL
Open Font License ([fonts/ttf/JetBrainsMono-OFL.txt](fonts/ttf/JetBrainsMono-OFL.txt),
[tests/fonts/LICENSE.txt](tests/fonts/LICENSE.txt)).
