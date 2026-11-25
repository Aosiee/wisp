# Wisp

[![Linux](https://github.com/Aosiee/wisp/actions/workflows/linux.yml/badge.svg)](https://github.com/Aosiee/wisp/actions/workflows/linux.yml)
[![Windows: not yet](https://img.shields.io/badge/Windows-not%20yet-lightgrey)](#building)
[![macOS: not yet](https://img.shields.io/badge/macOS-not%20yet-lightgrey)](#building)

A retained-mode UI library for real-time applications and the tools around
them: widgets, layout, popups and menus, drag and drop, animation, a
docking system with torn-out windows, a code editor and a document viewer.
Wisp builds each window's frame into draw batches; the program's renderer
records them.

Wisp stands on [Hearth](https://github.com/Aosiee/hearth) (the platform and
the host seam) and [tmath](https://github.com/Aosiee/tmath). It was written
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

## What the program supplies

Wisp draws nothing and opens no window by itself. The program gives it:

- **A renderer** (`Wisp/Renderer.h`): `IUIRenderer` creates the textures
  (the glyph atlas, images) and prepares each window's batches; the
  program records them in its own frame.
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
```

A host build adds Wisp with `add_subdirectory` and links `wisp::wisp` (or
`wisp::text` alone); a host that already has Hearth or tmath targets keeps
its own. `-DWISP_DEBUG_TOOLS=OFF` leaves out the widget reflector and the
focus overlay. The full suite, side by side and in every configuration,
runs through [hamr](https://github.com/Aosiee/hamr): `hamr run`
(`--tier nightly` adds the sanitizers).

## License

The Wisp License: MIT-style, with one condition: a product that includes
Wisp credits it in its credits or about box. See [LICENSE](LICENSE). The
suite's test face, Adwaita Mono, is under the SIL Open Font License
([tests/fonts/LICENSE.txt](tests/fonts/LICENSE.txt)).
