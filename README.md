<p align="center">
  <img src="media/logo-rosetta.png" alt="rosetta" width="400">
</p>

# Rosetta

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-26-blue.svg?logo=cplusplus" alt="C++26">
  <img src="https://img.shields.io/badge/status-prototype-yellow.svg" alt="Status: prototype">
  <a href="https://github.com/rosetta-bindings/rosetta/#1"><img src="https://img.shields.io/badge/slides-rosetta-blue?logo=marp" alt="Slides"></a>
  <a href="https://github.com/rosetta-bindings/rosetta/stargazers"><img src="https://img.shields.io/github/stars/xaliphostes/rosetta?style=social" alt="GitHub stars"></a>
</p>

<p align="center">
  <a href="https://github.com/bloomberg/clang-p2996"><img src="https://img.shields.io/badge/clang--p2996-tested-brightgreen.svg?logo=llvm" alt="clang-p2996: tested"></a>
  <img src="https://img.shields.io/badge/EDG-experimental-yellow.svg" alt="EDG: experimental">
  <img src="https://img.shields.io/badge/NVC%2B%2B-planned-lightgrey.svg?logo=nvidia" alt="NVC++: planned">
  <img src="https://img.shields.io/badge/GCC-in%20progress-lightgrey.svg?logo=gnu" alt="GCC: in progress">
  <img src="https://img.shields.io/badge/Clang%20%7C%20MSVC-tracking-lightgrey.svg" alt="Clang | MSVC: tracking">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/macOS-tested-brightgreen.svg?logo=apple" alt="macOS: tested">
  <img src="https://img.shields.io/badge/Linux-untested-lightgrey.svg?logo=linux&logoColor=white" alt="Linux: untested">
  <img src="https://img.shields.io/badge/Windows-untested-lightgrey.svg" alt="Windows: untested">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/binding languages-Python%20%7C%20Node%20%7C%20Wasm%20%7C%20TypeScript%20%7C%20Lua%20%7C%20Julia%20%7C%20Csharp%20%7C%20Java%20%7C%20Dynamic-green.svg" alt="Bindings">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/binding GUIs-Qt%20%7C%20QML%20%7C%20ImGUI-green.svg" alt="Bindings">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/binding others-ParaView%20%7C%20Json%20%7C%20Html%20%7C%20REST%20%7C%20OpenAPI%20%7C%20Markdown-green.svg" alt="Bindings">
</p>

A C++26 reflection playground with **19 generator backends** — Python (pybind11 / nanobind), Node, WebAssembly, Qt, QML, Dear ImGui, REST, Julia, Lua, OpenAPI, JSON, TypeScript, C#, Java, Markdown, HTML, ParaView, a runtime object model... bindings for **your existing classes — without modifying them**. Point rosetta at a header via a small [manifest.json](./docs/MANIFEST.md), run one tool, get per-language binding projects out.

> **Your target compiler doesn't support reflection?** Generate the expanded binding once on a Linux or macOS host with a C++26 / P2996 compiler — e.g. the [Bloomberg `clang-p2996`](https://github.com/bloomberg/clang-p2996) fork — then ship and build the generated sources anywhere with an off-the-shelf toolchain (plain Clang / GCC / MSVC, or an off-the-shelf emsdk for WebAssembly). No reflection is needed on the target (see **Reflection-free by construction** below).

Annotations (`doc`, `range`, `readonly`, …) are an *opt-in* enrichment, not a requirement: add them where you want docstrings, validation, or UI hints; leave the rest of the class alone. Reflection does the work either way.

## Features

<details>
<summary>Fields, methods, inheritance, constructors, enums, free functions, <code>std::vector</code> — discovered by reflection; opt-in annotations enrich, inline or out of line <i>(expand)</i></summary>

Everything below is discovered by **reflection** from your unmodified headers — you declare *what* to bind in [manifest.json](./docs/MANIFEST.md), never *how*.

**What rosetta can bind**

- **Public fields** — exposed as read/write properties, with per-backend getters/setters.
- **Public methods** — both instance and `static` members.
- **Inheritance** — public base-class fields and methods are flattened into the derived binding; a derived declaration shadows the base one (most-derived wins) and a virtual diamond collapses to a single member. Virtual / overriding methods are flagged (`virtual_spec`) so backends can tell them apart from plain ones.
- **Multiple constructors** — default *and* parameterized; each overload is bound.
- **Overloaded methods** — the whole set reaches the binding. Targets whose runtime dispatches on argument types (pybind11, nanobind, jlcxx) bind every overload; targets that key a method by name (embind, N-API, sol2, C#/Java, REST) bind the first-declared one and report the rest — see [overloads and coverage](docs/COVERAGE.md).
- **Enums** — `enum` / `enum class`, with enumerators surfaced as named constants.
- **Free (non-member) functions** — declared in the [manifest](./docs/MANIFEST.md), no edit to your headers ([details](docs/FREE_FUNCTIONS.md)).
- **Nested user types & `std::vector`** — `Surface` returning `Point`/`Triangle`, vector members, etc. are marshalled across the language boundary.
- **Parameter names & default arguments** — read from the declaration, so a binding offers `mesh.remesh(edge_length=0.5)` rather than `remesh(arg0)`, and a C++ default reaches the host language as one ([details](docs/MANIFEST.md#doc-comments-doc_comments)).
- **The documentation your library already has** — `///` and `/** @param … @return … */` blocks are read out of the headers and become Python docstrings, TSDoc, Javadoc, C# XML docs and OpenAPI descriptions. Nothing to write, nothing to annotate; turn it off with `"doc_comments": false`.
- Members a backend can't marshal (e.g. `std::function` params) are **skipped**, not fatal.

**Opt-in annotations** (enrich without intruding — see the [annotation reference](docs/ANNOTATIONS.md))

- `doc{"..."}` — docstrings / generated reference text.
- `readonly` — read-only property (write is rejected per backend).
- `range{lo, hi}` — value-range validation on assignment.
- `combobox{{...}}` — enumerated choices (UI hint).

Don't want to touch the header at all? Provide the same annotations **out of line**, from a JSON string attached to the type elsewhere — even in another file:

```cpp
struct MyClass {
    int value = 0;
};

// Add annotation later on in another file
template <>
constexpr std::string_view rosetta::ann_json_source<MyClass> = 
  R"({ "value": { "range": [1, 9] } })";
```

In a manifest-driven build you don't write that by hand: add an `"annotations": "Type.ann.json"` field to the class in `manifest.json` and rosetta bakes the external file in for you. Either way the metadata is merged with any inline annotations and reaches every backend (Python, Node, REST, OpenAPI, …) — see [out-of-line annotations](docs/OUT_OF_LINE_ANNOTATIONS.md).

</details>

## Backends (one combined module per target, from a single generator)

| # | Target | Ok |
|---|---|:---:|
| 1 | **Python** (`python`) — pybind11 extension module | ✅ | 
| 2 | **Python** (`nanobind`) — leaner/faster pybind11 successor | ✅ |
| 3 | **Node** (`node`) — N-API native addon | ✅ |
| 4 | **Julia** (`julia`) — CxxWrap.jl / jlcxx shared module, `std::vector` included | ✅ |
| 5 | **WebAssembly** (`wasm`) — Emscripten/embind module | ✅ |
| 6 | **Lua** (`lua`) — sol2 module, `require`-able (off-the-shelf C++17) | ✅ |
| 7 | **C#** (`csharp`) — native C-ABI shared library + handle-backed P/Invoke wrappers | ✅ |
| 8 | **Java** (`java`) — native C-ABI + handle-backed FFM wrappers | ✅ |
| 9 | **Qt Widgets** (`qt`) — generated property/method inspector via `runtime/qt_widgets.h` | ✅ |
| 10 | **QML** (`qml`) — fills a generic `ReflectedObject` explicitly | ✅ |
| 11 | **Dear ImGui** (`imgui`) — immediate-mode inspector app (GLFW + OpenGL3, auto-fetched) | ✅ |
| 12 | **REST** (`rest`) — cpp-httplib JSON server + generated browser client| — |
| 13 | **OpenAPI** (`openapi`) — OpenAPI 3.1 spec describing the REST surface | ✅ |
| 14 | **JSON** — reflection-based nlohmann (de)serialization (`visitors/json.h`) | — |
| 15 | **TypeScript** (`typescript`) — ambient `.d.ts` type declarations | ✅ |
| 16 | **Markdown** (`markdown`) — API reference document | ✅ |
| 17 | **HTML** (`html`) — self-contained, styled API reference page | ✅ |
| 18 | **ParaView** (`paraview`) — Server Manager XML for a plugin | ✅ |
| 19 | **Dynamic** (`dynamic`) — the IR as runtime *data*: a `MetaClass` per type + one thunk per member, queried and called by name ([details](examples/dynamic)) | ✅ |

> New backends register without touching the generator, thanks to the visitor pattern — see [EXTENDING_BACKEND](docs/EXTENDING_BACKEND.md).
> **C++26** = targets generated against the reflection toolchain.
> **C++20** = target builds with an off-the-shelf compiler (no reflection needed on the target).
>
> Notes:
> **Qt/QML** targets need Qt 6 but no moc on the generated code; 
> **lua** needs Lua 5.1–5.4 or LuaJIT (sol2 does not support Lua 5.5 yet) — sol2 itself is fetched automatically at configure time. 
> **REST** is the one target whose generated code still splices reflections, so it alone needs the C++26 toolchain to build.  
> **dynamic** is not a language binding: it emits the metadata itself, for a caller that discovers types at run time — see below.

**Reflection-free by construction.** Every binding target **fully expands** each field, method, constructor and enumerator into explicit pybind11 / nanobind / N-API / embind / Qt / sol2 / jlcxx / member-pointer calls (the `dynamic` backend expands them into aggregate-initialized tables and thunks instead — data rather than framework calls, but the same once-on-the-host rule). Reflection runs once, on the generation host; the generated binding is ordinary C++ that builds with an off-the-shelf toolchain — a plain C++17/20 compiler, emsdk, or Qt 6 (the host still needs C++26 to *run the generator*, the target does not). This pairs naturally with [out-of-line annotations](docs/OUT_OF_LINE_ANNOTATIONS.md) so the bound headers stay plain C++ too.

> Until 2026-08 seven of these languages shipped a second, *thin* backend whose generated code re-ran the reflection walk at the target's compile time. It has been removed: the short name (`python`, `nanobind`, `node`, `wasm`, `julia`, `csharp`, `java`) now means what `-expanded` used to. `qt`, `qml`, `imgui` and `lua` never had a thin twin at all, and have since dropped the suffix too. No target spells `-expanded` any more — but every old spelling still resolves, so existing manifests keep working.

**Python wheels.** `python` and `nanobind` also emit a `pyproject.toml` and a `make_wheel.py`, so a generated binding goes from source to an installable, redistributable wheel in one command — `python make_wheel.py`, the same on Linux, macOS and Windows. External shared libraries are bundled in and the platform tag repaired; on 3.12+ nanobind wheels are tagged `abi3`, covering every later CPython with a single artifact. See [Python wheels](docs/MANIFEST.md#python-wheels-version).

## Dynamic object model — reflect and invoke at run time

<details>
<summary>The <code>dynamic</code> backend emits the IR as <b>data</b>: ask what exists, call it by name, from a front-end that never includes your headers <i>(expand)</i></summary>

Every other backend turns your classes into calls into some framework. The **`dynamic`** backend turns them back into the IR — a `rosetta::dyn::MetaClass` per bound type plus one captureless-lambda thunk per member — so a program can ask *what exists* and *call it by name*, with no code generated for the caller (see [this example](./examples/dynamic/README.md)):

```cpp
#include <rosetta/dynamic.h>     // the runtime — ordinary C++20
#include "auto_dynamic.h"        // the generated tables (scene.h is NOT included)

scene::register_all();           // publish the tables into the registry, once

for (const rosetta::dyn::MetaClass *k : rosetta::dyn::registry().classes())
    for (std::size_t i = 0; i < k->n_fields; ++i)
        std::cout << k->qualified << "::" << k->fields[i].name << '\n';
        // ... plus .type, .doc, .readonly, .range, .choices — and .get / .set thunks
```

What that buys, relative to the language backends:

- **Overloads come back.** The metadata keeps the whole overload set and scores it against the actual arguments, so the name-keyed targets' first-only limit ([COVERAGE.md](docs/COVERAGE.md)) doesn't apply — and a failed call can name every candidate it rejected, with the reason.
- **UI by query, not by codegen.** A property inspector is a walk over `registry()`: `label` → caption, `doc` → tooltip, `range` → slider bounds, `readonly` → disabled row, `combobox` / enumerators → drop-down, `button` → action row. Annotations are enforced once, in the core, instead of per backend.
- **What can't be marshalled is described, not deleted** — a `std::function` parameter stays in the metadata with the reason it is unavailable, so a UI can grey it out.
- **Lifetime is explicit** — a `T&` return crosses without a copy and *pins* its parent, so a sub-object handle cannot outlive the object it points into.

The generated tables are aggregate initializers, so like the other targets they build with an **off-the-shelf C++20 compiler**. The per-call cost (name lookup, overload scoring, `Any` marshalling) makes this the right tool for control — properties, commands, menus, scripting glue — and it wants a cache in front of it for bulk data; [`examples/dynamic`](examples/dynamic) measures both and shows a terminal interpreter and a Qt viewer (3D view, property panel, console) driving one set of tables without naming a single bound type.

</details>

## Mini-MOC — Qt signals / slots / properties, without moc

<details>
<summary>A header-only, <b>moc-less</b> take on signals/slots/properties, built on reflection + annotations — annotate members, <code>connect&lt;"sig","slot"&gt;</code>, done <i>(expand)</i></summary>

Beyond binding generation, rosetta ships [`mini_moc.h`](include/rosetta/mini_moc.h): a header-only, **moc-less** reimagining of Qt's signals/slots/properties built directly on C++26 reflection (P2996) + annotations (P3394). No code generator, no separate compile step — just annotate members and connect them.

You mark members with annotations and reach them through three free functions:

```cpp
#include <rosetta/mini_moc.h>
namespace moc = rosetta::moc;

class Person {
public:
    moc::Signal<std::string const &> nameChanged;   // a Signal<...> member IS a signal
    moc::Signal<int>                 ageChanged;

    [[= moc::property{"name", "nameChanged"}]] std::string m_name;
    [[= moc::property{"age",  "ageChanged"}]]  int         m_age = 0;
    [[= moc::property{"id"}]]                  int         m_id  = 0;   // no NOTIFY
};

struct Logger {
    [[= moc::slot]] void onAge(int v)               { total += v; }
    [[= moc::slot]] void onName(std::string const&) { /* ... */ }
    int total = 0;
};

Person p; Logger l;
moc::connect<"ageChanged", "onAge">(p, l);  // compile-time checked
moc::set<"age">(p, 30);                      // equality-gated; fires NOTIFY -> Logger::onAge
moc::get<"age">(p);                          // -> 30
```

- **Signals need no annotation** — any `Signal<...>` data member is a signal, recognized by its type. `slot` and `property{"name", "notifySig"}` annotations mark the members that *aren't* self-identifying; reflection discovers them.
- **`connect<"sig","slot">(sender, receiver)`** — compile-time checked: a wrong name is a `static_assert`, mismatched argument types are a template error.
- **`get<"prop">` / `set<"prop">`** — property access from outside the class (token injection, P3294, isn't in clang-p2996 yet, so accessors aren't emitted into the class body). `set<>` is equality-gated and fires the property's `NOTIFY` signal only on an actual change.
- **`Signal<Args...>`** — the only machinery type you spell out; supports `connect` / `disconnect` / `disconnect_all`, re-entrant self-disconnect, and a `ScopedConnection` RAII handle for scope-bound connections.

See the [`examples/moc`](examples/moc) demo and the test suite in [`tests/moc.cpp`](tests/moc.cpp).

</details>

## Status

<details>
<summary>Prototype — tracks P2996 / P3394 / P3294; <b>clang-p2996 today</b>, EDG the likely next target <i>(expand)</i></summary>

Prototype. Tracks the in-flight C++26 reflection papers:

- **P2996** — reflection (`^^T`, `[: r :]` splice, `std::meta::*`)
- **P3394** — annotation attributes (`[[= rosetta::doc{"..."}]]`)
- **P3294** — token injection (not yet used; see notes in `mini_moc.h`)

Builds with the Bloomberg [clang-p2996 fork](https://github.com/bloomberg/clang-p2996) — the reference implementation rosetta is developed and tested against.

No mainline compiler ships these proposals yet, but other front-ends are implementing P2996 and should become viable targets as their support matures (and as rosetta's compiler-specific flags are abstracted):

- **clang-p2996** (Bloomberg fork) — ✅ supported today; what rosetta is built and tested with.
- **EDG** — front-end has an experimental P2996 implementation; the most likely next target.
- **NVC++ / NVHPC** — built on the EDG front-end, so it could inherit reflection as EDG's support lands in releases.
- **GCC** — reflection is under active development on experimental branches; not yet usable for rosetta.
- **Mainline Clang / MSVC** — tracking P2996 but no usable implementation yet.

Annotations (P3394) and token injection (P3294) are newer and currently exist only in the clang-p2996 fork, so full-feature builds remain fork-only for now.

</details>

## Requirements

<details>
<summary>A clang-p2996 build, CMake 3.28+, and the fork's reflection flags <i>(expand)</i></summary>

- A clang-p2996 build at `$HOME/devs/c++/clang-p2996/build` (or override `CLANG_P2996_ROOT` when invoking cmake).
- CMake 3.28+, Ninja or Make.
- C++26 mode with the fork's flags: `-freflection -freflection-latest -fexperimental-library`. Annotation-using code also needs `-fannotation-attributes`.

</details>

## Build the test suite

<details>
<summary><code>cd tests && cmake -B build && cmake --build build</code> — each test is a self-contained binary <i>(expand)</i></summary>

```bash
cd tests
cmake -B build
cmake --build build
./build/hello
./build/moc
./build/docgen
# ...
```

Each test is self-contained; pick by name (see `tests/CMakeLists.txt`).

</details>

## A taste — bind your existing library

<details>
<summary>An untouched <code>person.h</code> + a small <code>manifest.json</code> → Python and Node modules in four commands <i>(expand)</i></summary>

You have this header. Don't change it:

```cpp
// my_lib/person.h
#include <string>

struct Person {
    std::string name;
    int         age = 0;
    std::string id;
    std::string greet(const std::string &salutation) const;
};
```

Write a small [manifest.json](./docs/MANIFEST.md) next to it. Each `targets` entry names the module/library produced for that backend; list every class you want bound:

```json
{
  "user_include": "../my_lib",
  "rosetta_include": "/path/to/rosetta/include",
  "targets": [
    { "lang": "python", "name": "person_py" },
    { "lang": "node",   "name": "person_js" }
  ],
  "classes": [
    { "name": "Person", "header": "person.h" }
  ]
}
```

Once the scaffolder is built (first block below), one command runs the rest of the pipeline — generation, generator build, and a compile of every backend (skipping any whose toolchain is missing):

```bash
/path/to/rosetta/bin/rosetta_gen --build manifest.json    # → bindings/, compiled; --help lists the options
```

Every `rosetta_gen` mode and option (`--build`, `--clean`, `--init`, plain mode) is documented with worked cases in [`docs/ROSETTA_GEN.md`](./docs/ROSETTA_GEN.md).

Or step by step — generate, build, and run the project-specific tool it emits:

```bash
# (one-time) build the framework scaffolder → <repo>/bin/rosetta_gen
cmake -S tools/rosetta_gen -B tools/rosetta_gen/build
cmake --build tools/rosetta_gen/build

# from the folder holding manifest.json:
#   write the generator project (bindings.h + <generator_name>.cpp + CMakeLists.txt)
#   into a folder you name — here `gen/`
/path/to/rosetta/bin/rosetta_gen manifest.json gen

# build it — the `generator` binary is dropped into the current folder,
# not the build tree
cmake -S gen -B gen/build && cmake --build gen/build

# run it → one combined module per backend under bindings/
./generator bindings
```

Result: `bindings/{python,node}/` — each a self-contained CMake project exposing **all** your classes in a single module. `cd bindings/python && cmake -B build && cmake --build build`, then `import person_py`.

> `generator_name` and `module_name` are optional manifest fields: `generator_name` (the generated `.cpp` / usage name) defaults to the manifest's folder name, and a bare-string target like `"node"` falls back to `module_name` for its module name.

The full walkthrough is in [`docs/QUICKSTART.md`](./docs/QUICKSTART.md); every manifest field is documented in [`docs/MANIFEST.md`](./docs/MANIFEST.md); the `rosetta_gen` tool and its modes are in [`docs/ROSETTA_GEN.md`](./docs/ROSETTA_GEN.md); the `binding_info<T>` trait and the layered tooling model are in [`docs/GENERATE.md`](./docs/GENERATE.md). The worked examples live in `examples/manifest/` and `examples/geom-lib/`.

</details>

## Extending a generated binding in C++

<details>
<summary>Add a hand-written registration block <b>beside</b> the generated source — regeneration never clobbers your extensions <i>(expand)</i></summary>

Everything under `bindings/` is regenerated output — never edit it. When the generated binding misses something you need (a helper the walker skips, such as an overloaded free function; a custom view over a type that isn't bound; a typed-array export for a renderer), add it in a **separate hand-written C++ file** and compile it *alongside* the generated source, from a small build of your own that lives outside `bindings/`. The binding frameworks accept several registration blocks per module, so your file simply contributes a second one — nothing generated is touched, and regenerating the bindings never clobbers your extensions.

A complete worked example is [`pmp-rosetta/wasm-viz`](https://github.com/rosetta-bindings/pmp-rosetta/tree/main/wasm-viz): a stand-alone WebAssembly build for a three.js viewer that compiles the generated `bindings/wasm/auto_emscripten.cpp` verbatim plus one hand-written `viz_helpers.cpp`, which adds what the auto-generated binding does not expose — flat vertex/index buffers as JS typed arrays, and a wrapper for an overloaded function the generator skips — in its own `EMSCRIPTEN_BINDINGS` block:

```cpp
// viz_helpers.cpp — compiled next to the generated auto_emscripten.cpp
emscripten::val mesh_positions(const pmp::SurfaceMesh &mesh);  // Float32Array
emscripten::val mesh_triangles(const pmp::SurfaceMesh &mesh);  // Uint32Array
void triangulate_mesh(pmp::SurfaceMesh &m) { pmp::triangulate(m); } // overload → skipped by the generator

EMSCRIPTEN_BINDINGS(pmp_viz) { // second block; the generated one keeps its own
    emscripten::function("mesh_positions", &mesh_positions);
    emscripten::function("mesh_triangles", &mesh_triangles);
    emscripten::function("triangulate_mesh", &triangulate_mesh);
}
```

Embind is the friendliest here because it accepts any number of `EMSCRIPTEN_BINDINGS` blocks in one module. Backends with a single module entry point (pybind11's `PYBIND11_MODULE`, N-API's `NODE_API_MODULE`) can't add a second block to the *same* module — there, ship your extras as a small companion module built the same way (it sees the same user headers and links the same library), and import/require both.

</details>

## Examples

<details>
<summary><b>19 worked examples</b> — manifest-driven, expanded targets, the dynamic object model, trampolines, mini-moc, the three UI inspectors, hand-written references <i>(expand)</i></summary>

| Path                       | What it shows                                       |
|----------------------------|-----------------------------------------------------|
| `examples/manifest`        | Manifest-driven generation for `Person` (no class modification) |
| `examples/annotate-manifest`| Out-of-line annotations from an external JSON file, wired by the manifest's `annotations` field ([details](docs/OUT_OF_LINE_ANNOTATIONS.md)) |
| `examples/doxygen`         | The documentation a library already has: plain Doxygen comments and default arguments in an untouched header become Python docstrings + keyword arguments, TSDoc, OpenAPI descriptions and a Markdown reference ([details](docs/MANIFEST.md#doc-comments-doc_comments)) |
| `examples/geom-lib`        | Manifest-driven bindings for a small geometry library (nested types, vectors) |
| `examples/geom-expanded`   | Reflection-free `python` / `nanobind` / `node` / `wasm` / `qt` / `qml` / `csharp` / `java` / `lua` / `julia` bindings (off-the-shelf compiler, emsdk and Qt, any Lua 5.1–5.4, CxxWrap.jl) with out-of-line annotations |
| `examples/dynamic`         | The `dynamic` backend end to end: one set of generated metadata driving a terminal interpreter *and* a Qt viewer (3D view + property panel + console), neither naming a bound type |
| `examples/trampoline-python` | Overriding C++ virtuals from Python — generated pybind11 trampolines from `virtual_spec` |
| `examples/trampoline-node` | Overriding C++ virtuals from JavaScript — generated N-API trampolines from `virtual_spec` |
| `examples/moc`             | Qt-flavoured meta-object demo on `mini_moc.h` (properties + signals) |
| `examples/docgen`          | Reflection-driven Markdown / HTML reference generator |
| `examples/paraview`        | ParaView plugin property-panel XML from an annotated `vtkThreshold` spec (every backend feature) |
| `examples/qt`              | Building a Qt widget form from a reflected struct   |
| `examples/qml`             | Exposing a reflected C++ object to QML              |
| `examples/imgui`           | Dear ImGui inspector for `Algo` with out-of-line annotations (`Algo.ann.json`: `doc` / `range` / `combobox`) — builds with an off-the-shelf C++20 compiler, deps auto-fetched |
| `examples/bindings/python` | Hand-written pybind11 backend (reference)           |
| `examples/bindings/node`   | Hand-written N-API backend (reference)              |
| `examples/bindings/julia`  | Hand-written CxxWrap/jlcxx backend (reference, requires CxxWrap.jl) |
| `examples/bindings/rest`   | Hand-written HTTP/REST backend (reference)          |
| `examples/bindings/web`    | Hand-written WebAssembly backend (requires reflection-aware emsdk) |

</details>

## Rosetta in the wild

<details>
<summary>Real libraries bound with rosetta — <b>PMP, geogram, Arch, Cassini</b> — each from a single <code>manifest.json</code>, no hand-written wrappers <i>(expand)</i></summary>

| Project | Bound library | Targets |
|---|---|---|
| [pmp-rosetta](https://github.com/rosetta-bindings/pmp-rosetta) | [PMP](https://www.pmp-library.org) — the Polygon Mesh Processing library (remeshing, smoothing, subdivision, decimation) | Python, Node.js, WebAssembly, TypeScript |
| [geogram-rosetta](https://github.com/rosetta-bindings/geogram-rosetta) | [geogram](https://github.com/BrunoLevy/geogram) — Bruno Lévy's geometry-processing library (reconstruction, remeshing, parameterization, booleans/CSG) | Python, Node.js, WebAssembly, TypeScript, Lua |
| [arch-rosetta](https://github.com/rosetta-bindings/arch-rosetta) *(private)* | Arch — a 3-D boundary-element (BEM) geomechanics code | Python, Node.js, WebAssembly, TypeScript |
| [cassini-rosetta](https://github.com/rosetta-bindings/cassini-rosetta) *(private)* | Cassini — FEM geomechanical restoration, bound through a single high-level C++ facade | Python, Node.js, WebAssembly, TypeScript |
| [triax-rosetta](https://github.com/rosetta-bindings/triax-rosetta) | Triax — a discrete-element (DEM) triaxial compression simulator for granular packings | Python, Node.js, WebAssembly |

</details>

## Design notes

<details>
<summary>Quick start, the manifest / generate / annotation references, and the todo list <i>(expand)</i></summary>

- [**The rosetta book**](docs/book) — the whole manual as a single PDF (~105 pages): what rosetta is, the pipeline, the complete manifest reference, annotations, the tool, the runtime object model, writing a backend, worked examples, and four reference appendices. `cd docs/book && make`
- [Quick start](docs/QUICKSTART.md) — five-step guide to generating bindings for an existing library
- [Extending](docs/EXTENDING_BACKEND.md) — how to extend the rosetta backend
- [Manifest](docs/MANIFEST.md) — complete reference for `manifest.json`: every field, target and option
- [Annotations](docs/ANNOTATIONS.md) — the full annotation reference (doc / range / readonly / combobox / label / button / widget hints, inline & out-of-line)
<br><br>
- [Generate](docs/GENERATE.md) — full reference for `rosetta::generate`, the manifest schema, and the tool layering
- [Free functions](docs/FREE_FUNCTIONS.md) — sketch for reflecting namespace-scope functions
- [Other annotations](docs/OTHER_ANNOTATIONS.md) — proposed annotation kinds beyond the current set
- [Out-of-line annotations](docs/OUT_OF_LINE_ANNOTATIONS.md) — keep headers clean: a JSON side-car of annotations baked in at generation time, merged at compile time
- [Todo list](docs/TODO.md) — what the walker and visitor surface still miss (static data members, parameter metadata, nested types, ...)

</details>

## License

[MIT](./LICENSE)

## Author
[xaliphostes](https://github.com/xaliphostes)