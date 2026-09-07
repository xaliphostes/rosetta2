# `manifest.json` — the rosetta manifest

The **manifest** is the single hand-written file that drives rosetta. You point it at your existing C++ headers, list the languages you want bindings for, and `rosetta_gen` does the rest. Your class definitions are never modified.

It answers three questions for the framework:

1. **Where** are your headers and where is rosetta's `include/`?
2. **What** classes / free functions should be bound?
3. **Which** language backends (targets) should be emitted?

Everything else — fields, methods, constructors, enums, inheritance — is discovered by C++26 reflection from the headers themselves. You declare *__what__* to bind, never *__how__*.

---

## Contents

Every field is listed in [Top-level fields](#top-level-fields); the sections below it explain the ones that need more than a table cell.

- [Where it fits](#where-it-fits)
- [Minimal example](#minimal-example)
- [**Top-level fields**](#top-level-fields)
- [Variables (`variables`)](#variables-variables)
  - [What is *not* substituted](#what-is-not-substituted)
- [Presets (`preset`)](#presets-preset)
  - [What it does](#what-it-does)
  - [Shipped presets](#shipped-presets)
- [Classes](#classes)
- [Binding a whole folder (`header` as a glob)](#binding-a-whole-folder-header-as-a-glob)
  - [The stem is the class name](#the-stem-is-the-class-name)
- [Renaming a class (`expose`)](#renaming-a-class-expose)
- [Shared defaults (`namespace`, `header_dir`)](#shared-defaults-namespace-header_dir)
  - [Grouped entries](#grouped-entries)
- [Extension methods (`extensions`)](#extension-methods-extensions)
- [Pinning the runtime (`python`, `requires_python`, `napi_version`, `node_engine`)](#pinning-the-runtime-python-requires_python-napi_version-node_engine)
- [Artifact output directory (`out_dir`)](#artifact-output-directory-out_dir)
- [Foreign sequence containers (`sequences`)](#foreign-sequence-containers-sequences)
  - [Registering a concrete type](#registering-a-concrete-type)
- [Foreign matrices (`matrices`)](#foreign-matrices-matrices)
- [Foreign-library interop (`interop`)](#foreign-library-interop-interop)
  - [Reaching the caster-less backends: register the type as a sequence too](#reaching-the-caster-less-backends-register-the-type-as-a-sequence-too)
- [Standard types that need no declaration: `std::filesystem::path`, `std::shared_ptr<T>`](#standard-types-that-need-no-declaration-stdfilesystempath-stdshared_ptrt)
- [Doc comments (`doc_comments`)](#doc-comments-doc_comments)
- [Out-parameters (`out_params`)](#out-parameters-out_params)
- [Module init (`module_init`)](#module-init-module_init)
- [Generated headers (`generated_headers`)](#generated-headers-generated_headers)
- [Multiple include directories](#multiple-include-directories)
- [Functions](#functions)
  - [Binding one overload (`signature`)](#binding-one-overload-signature)
- [Targets](#targets)
  - [Available `lang` values](#available-lang-values)
- [Linking external libraries (`user_lib`)](#linking-external-libraries-user_lib)
  - [Several libraries: your library and its dependencies](#several-libraries-your-library-and-its-dependencies)
- [Compiling user sources (`user_sources`)](#compiling-user-sources-user_sources)
- [Preprocessor definitions (`compile_definitions`)](#preprocessor-definitions-compile_definitions)
- [Build type & optimization (`build_type`, `optimization`)](#build-type--optimization-build_type-optimization)
- [Python wheels (`version`)](#python-wheels-version)
- [Full reference example](#full-reference-example)
- [Common gotchas](#common-gotchas)

---

## Where it fits

```
manifest.json ──► rosetta_gen ──► generated/  ──cmake──► <generator_name>
                  (framework)     bindings.h              (project tool)
                                  <generator_name>.cpp        │
                                  CMakeLists.txt              run
                                                              ▼
                                                      output/python  node  wasm …
                                                      (per-backend project trees)
```

`rosetta_gen` reads the manifest and emits a project-specific generator; that generator, when run, emits one self-contained CMake project per target. Paths inside the manifest are resolved **relative to the manifest file** — move the file and you must re-run `rosetta_gen`.

---

## Minimal example

Start from a blank binding project scaffolded by `tools/rosetta_init.py` — it writes a `rosetta/` folder next to your library (`manifest.json` skeleton, a bootstrap `CMakeLists.txt` that fetches rosetta into `extern/` and builds `rosetta_gen`, `.gitignore`, `README.md`).

Download the script for scaffolding: [**rosetta_init.py**](https://github.com/rosetta-bindings/rosetta/blob/main/tools/rosetta_init.py), then:

```bash
# 1. scaffold an empty starting project inside (or next to) your library
python3 rosetta_init.py --dir my_lib/rosetta --name my_lib
```

```sh
cd my_lib/rosetta
```

Fill in the generated `manifest.json` — the scaffold leaves `classes` empty; add the types you want bound:

```json
{
  "user_include": ["./include"],
  "user_sources": ["./src/*.cxx"],
  "rosetta_include": "./extern/rosetta/include",
  "generator_name": "my_lib_gen",
  "targets": ["python", "node", "rest", "wasm"],
  "classes": [
    { "name": "Person", "header": "person.h" }
  ]
}
```

Build & run:

```bash
# 2. one-time bootstrap: fetch rosetta into extern/ and build rosetta_gen
#    (binary lands in extern/rosetta/bin)
cmake -B build && cmake --build build

# 3. the whole pipeline in one command: emit + build + run the generator,
#    then compile every backend the manifest declares
./extern/rosetta/bin/rosetta_gen --build manifest.json
```

`--build --help` lists the options (`--only`/`--skip` backends, `--jobs`, `--fresh`, …), and `rosetta_gen --clean manifest.json` removes everything it generated. Every mode and option of the tool — including running the steps `--build` automates by hand — is documented in [ROSETTA_GEN.md](ROSETTA_GEN.md).

---

## Top-level fields

| Field | Required | Default | Meaning |
|---|:---:|---|---|
| `user_include` | ✅ | — | Directory holding your class headers — **or an array of directories** when they live in several places. Each entry is relative to the manifest, or absolute, and resolved to an absolute path. See [Multiple include directories](#multiple-include-directories). |
| `rosetta_include` | ✅ | — | Path to rosetta's `include/` directory. Same resolution rules. |
| `generator_name` | — | manifest's directory name | Basename of the emitted reflection driver: `"my_person_gen"` ⇒ `gen/my_person_gen.cpp`. The built binary is always `generator`, so this is mostly cosmetic — its one load-bearing role is being the last-resort default for `module_name`. The **derived** default is folded to an identifier (`my-cool-lib` ⇒ `my_cool_lib`) so that fallback cannot produce an invalid module name; an explicit value is used verbatim. |
| `module_name` | — | a `preset`'s, else `generator_name` | Default binding module name, used by any **shorthand** (bare-string) target. A [preset](#presets-preset) may supply one, which is why a preset-based manifest usually names neither this nor `generator_name`. |
| `variables` | — | `[]` | Manifest-local variables, substituted as `$NAME` / `${NAME}` into every string of the manifest — a shared path prefix written once instead of once per field. See [Variables](#variables-variables). |
| `targets` | ✅ | — | The language backends to emit. See [Targets](#targets). |
| `classes` | ✅* | — | The classes / structs / enums to bind. See [Classes](#classes). *Optional when a [`preset`](#presets-preset) supplies them. |
| `functions` | — | `[]` | Free (non-member) functions to bind. See [Functions](#functions). |
| `preset` | — | — | A named JSON fragment shipped with rosetta that supplies a fixed binding surface, so the manifest states only what is project-specific. See [Presets](#presets-preset). |
| `namespace` | — | — | Default C++ namespace for `classes` / `functions` / `extensions` names carrying no `::` of their own. See [Shared defaults](#shared-defaults-namespace-header_dir). |
| `header_dir` | — | — | Directory fragment prepended to every `classes` / `functions` / `extensions` header. See [Shared defaults](#shared-defaults-namespace-header_dir). |
| `sequences` | — | `[]` | Foreign sequence containers that marshal like `std::vector<T>` — a qualified template name with one type parameter (`"GEO::vector"`), or a concrete type spelled exactly (`{ "type": "Eigen::VectorXd" }`). See [Foreign sequence containers](#foreign-sequence-containers-sequences). |
| `python` / `requires_python` / `napi_version` / `node_engine` | — | — | Per-target runtime pins: which Python the binding is built for, its minimum version, the N-API level and the npm `engines.node` entry. See [Pinning the runtime](#pinning-the-runtime-python-requires_python-napi_version-node_engine). |
| `wheel` / `wheel_dir` | — | — | **Per-target** (`python` / `nanobind` only): build a wheel for this target on every `--build`, and where it lands. See [Python wheels](#python-wheels-version). |
| `out_dir` | — | — | Where every target's **built artifact** is copied after each build (the `.so` / `.pyd` / `.node` / `.js`+`.wasm`). Per-target `out_dir` overrides it. See [Artifact output directory](#artifact-output-directory-out_dir). |
| `matrices` | — | `[]` | Foreign 2-D matrices (same two entry forms as `sequences`) that marshal as an array of row arrays. See [Foreign matrices](#foreign-matrices-matrices). |
| `interop` | — | `[]` | Foreign libraries whose types the target's binding framework marshals itself (`["eigen"]`). `python` / `nanobind` bind them natively (numpy); backends with no caster skip those members. See [Foreign-library interop](#foreign-library-interop-interop). |
| `user_lib` | — | — | Link the generated bindings against pre-built external libraries — one object, or an array of them when your library has dependencies of its own. See [Linking external libraries](#linking-external-libraries-user_lib). |
| `user_sources` | — | `[]` | List of user `.cpp` (or `.c`) files compiled directly into every generated binding target. See [Compiling user sources](#compiling-user-sources-user_sources). |
| `compile_definitions` | — | `[]` | Preprocessor definitions (`"NAME"` or `"NAME=VALUE"`) applied to the generator driver and every compiled binding target. See [Preprocessor definitions](#preprocessor-definitions-compile_definitions). |
| `build_type` | — | `Release` | Default `CMAKE_BUILD_TYPE` baked into every compiled backend's generated `CMakeLists.txt` (`Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`). See [Build type & optimization](#build-type--optimization-build_type-optimization). |
| `optimization` | — | — | Explicit optimization flag (`-O0`…`-O3`, `-Os`, `-Oz`, `-Og`, `-Ofast`) applied to every compiled backend, overriding the build type's own `-O` level. See [Build type & optimization](#build-type--optimization-build_type-optimization). |
| `version` | — | `0.1.0` | Distribution version stamped into the packaging artifacts — the `pyproject.toml` the `python` / `nanobind` backends emit for wheel builds. See [Python wheels](#python-wheels-version). |
| `plugins` | — | `[]` | Extra `.cpp` sources to compile into the generator driver (e.g. a custom backend). Paths relative to the manifest. |
| `doc_comments` | — | `true` | Read the Doxygen documentation and default arguments already present in the bound headers, and carry them into every backend. Set `false` to generate from annotations alone. See [below](#doc-comments-doc_comments). |
| `out_params` | — | `{}` | Which parameters a method returns **through a reference**, keyed `"Class::method"`. Never inferred. See [below](#out-parameters-out_params). |
| `module_init` | — | — | Statements the generated module runs when it **loads** (plus the headers declaring them) — a library's lifecycle, which is not a binding. See [below](#module-init-module_init). |
| `generated_headers` | — | `[]` | Headers the bound library's own build system would have produced (e.g. a configured `version.h`), written into the generated tree and put first on the include path. See [below](#generated-headers-generated_headers). |
| `qt_dir` | — | a built-in path | Qt 6 install prefix used by the `qt` / `qml` backends. e.g. `"$ENV{HOME}/Qt/6.8.3/macos"`. |
| `cpp26_root` | — | `$ENV{HOME}/devs/c++/clang-p2996/build` | Root of the C++26 / P2996 reflection toolchain. The generator always needs it; of the targets, only `rest` does. Moves `cpp26_cxx` / `cpp26_cc` / `cpp26_lib` together. |
| `cpp26_cxx` | — | `${cpp26_root}/bin/clang++` | C++ compiler (name or path) for the reflection toolchain. |
| `cpp26_cc` | — | `${cpp26_root}/bin/clang` | C compiler (name or path). |
| `cpp26_lib` | — | `${cpp26_root}/lib` | Directory holding the fork's `libc++` / `libc++abi` (`-L` / rpath). |

Keys beginning with `//` (e.g. `"//1"`, `"//note"`) are treated as comments and ignored — handy since JSON has no comment syntax.

---

## Variables (`variables`)

A path that appears in four fields is written four times, and moving it means
editing four lines and hoping none was missed. The `cpp26_*` block is the case
that motivated this — one toolchain root, spelled out with a different suffix
each time:

```json
"cpp26_root": "$ENV{HOME}/devs/c++/clang-p2996/build",
"cpp26_cxx":  "$ENV{HOME}/devs/c++/clang-p2996/build/bin/clang++",
"cpp26_cc":   "$ENV{HOME}/devs/c++/clang-p2996/build/bin/clang",
"cpp26_lib":  "$ENV{HOME}/devs/c++/clang-p2996/build/lib",
```

`variables` declares a name for the shared part, which `$NAME` (or `${NAME}`)
then stands for:

```json
"variables": [
    { "name": "P2996", "value": "$ENV{HOME}/devs/c++/clang-p2996" }
],

"cpp26_root": "$P2996/build",
"cpp26_cxx":  "$P2996/build/bin/clang++",
"cpp26_cc":   "$P2996/build/bin/clang",
"cpp26_lib":  "$P2996/build/lib",
```

The substitution runs over **every string in the manifest** before any field is
read, so a variable works anywhere a string does — `user_include`, `out_dir`,
a `user_lib` directory, a `compile_definitions` value — not only in the
toolchain paths above. The field itself disappears afterwards; nothing
downstream sees it.

Declarations are an **array**, not an object, because they are **ordered**: a
value may use the variables declared before it (and only those — a forward
reference is left as written).

```json
"variables": [
    { "name": "TC",  "value": "/opt/toolchains/clang-p2996" },
    { "name": "BIN", "value": "$TC/build/bin" }
],
"cpp26_cxx": "$BIN/clang++"
```

### What is *not* substituted

A manifest already carries two other dollar notations, and both belong to
**CMake**, which expands them at its own configure time. Rosetta must hand them
over untouched, so the rule is that **only a name you declared is substituted**:

| Written | Result |
|---|---|
| `$ENV{HOME}` | passed through verbatim — CMake's environment lookup, whatever `variables` says |
| `${CMAKE_SOURCE_DIR}` | passed through verbatim — no `CMAKE_SOURCE_DIR` was declared |
| `$P2996` with `P2996` declared | substituted |

Declaring `ENV` is an error, since `$ENV{...}` could never resolve to it.

The price of that leniency is that a **misspelt variable is silent** rather than
an error — `$P2996x` is one name, `P2996x`, which nothing declares, so it stays
as written and surfaces later as a path that does not exist. `${P2996}x` is how
a variable abuts other text, and is worth preferring for that reason.

A name is a C identifier: letters, digits and `_`, not starting with a digit.

---

## Presets (`preset`)

Some binding surfaces are **fixed**: they do not depend on your project at all.
Binding rosetta's own reflection API is the case that motivated this — the
twelve `rosetta::script` handle classes and the eight registry free functions
are the same in every project that wants them, and copying twenty entries into
each manifest makes the three lines that *are* project-specific hard to find.

`preset` names a JSON fragment shipped under
`<rosetta_include>/rosetta/presets/<name>.json`:

```json
{
    "preset": "scriptable",
    "rosetta_include": "../../include",
    "user_include": ["../lib", "../lib/bindings/dynamic"],
    "user_sources": ["../lib/bindings/dynamic/auto_dynamic.cpp"],
    "module_init": { "headers": ["auto_dynamic.h"], "statements": ["bank::register_all()"] },
    "targets": ["python", "lua"]
}
```

That is a complete manifest — six keys, every one of them about *this* project.
`classes`, `functions` and `module_name` all come from the preset; a preset that
supplies `module_name` is what lets the targets stay bare strings.

Give an array to apply several (`"preset": ["scriptable", "..."]`), applied in
order.

### What it does

| | |
|---|---|
| **Merge** | Deep, with the **manifest winning** every conflict it can express: a key you did not write is taken from the preset; arrays present in both are concatenated *preset first*; objects merge recursively; a scalar you spelled out is never overridden. The array rule is what lets `module_init` compose — the preset contributes its `headers` entry and your `statements` survive. |
| **`classes` / `functions`** | Parsed with a **pristine context**: your `namespace` and `header_dir` defaults describe *your* tree and are not prepended to a preset's headers. They are appended after your own entries. |
| **Include path** | A preset binds headers that live under `rosetta_include`, so it puts that directory on the **target's** include path too. Every other binding is reflection-free by construction and has no reason to see rosetta's headers, which is why this is not the default. |

`classes` becomes optional when a preset supplies it. An unknown name is an
error naming the path that was searched, so a typo surfaces immediately.

### Shipped presets

| name | what it binds |
|---|---|
| `scriptable` | [`<rosetta/script.h>`](../include/rosetta/script.h) — the bindable facade over the runtime object model — plus the per-language `Value` casters. Lets a host language walk the metadata of any library that emits `dynamic` tables and call into it by name. See [examples/scriptable-model](../examples/scriptable-model). |

A preset is only a JSON fragment: drop your own into
`<rosetta_include>/rosetta/presets/` to share a binding surface across projects.

---

## Classes

`classes` is an array of per-class entries. Each binds one C++ type (`class`, `struct`, or `enum`).

```json
"classes": [
  { "name": "Model", "header": "Model.h", "doc": "the model class" },
  { "header": "Point.h" },
  { "name": "space::Vector3", "header": "Vector3.h", "annotations": "Vector3.ann.json" }
]
```

| Field | Required | Default | Meaning |
|---|:---:|---|---|
| `header` | ✅ | — | Filename emitted into `#include "..."`. Resolved against `user_include`. May be a **glob** (`"*.h"`), which binds every header it matches — see [Binding a whole folder](#binding-a-whole-folder-header-as-a-glob). |
| `name` | — | header basename | C++ type name, must be reachable after including `header`. May be namespace-qualified (`space::Vector3`). |
| `expose` | — | unqualified `name` | The **binding name** — what scripts see (Python attribute, JS export, TypeScript class) and what the generated trampoline is suffixed with (`Py_<expose>` / `Js_<expose>`). Works on an enum entry too. Must be a plain identifier. See [Renaming a class (`expose`)](#renaming-a-class-expose). |
| `annotations` | — | — | Path (relative to the manifest) to an out-of-line annotation JSON side-car, baked into `bindings.h` so the header stays clean. See [OUT_OF_LINE_ANNOTATIONS](OUT_OF_LINE_ANNOTATIONS.md). |
| `doc` | — | — | A description string for the class (used by doc backends). |
| `extensions` | — | `[]` | Free functions exposed as **instance methods** of this class. See [Extension methods](#extension-methods-extensions). |
| `final` | — | `false` | Treat the class as non-overridable from the host language: **no trampoline** is generated even when it has public virtual methods (they still bind as ordinary callable methods). Also what makes a class *with* virtuals eligible as a node member-object property (`mesh.vertices` — the aliased wrap stores a `T*`, which requires the wrapped type to be `T`, not `Js_T`). |

Inheritance, multiple constructors, enums, nested user types and `std::vector` members are discovered automatically — no entry needed per base class, just list the most-derived type you want bound.

---

## Binding a whole folder (`header` as a glob)

Optional shorthand for the common library shape — *one class per header, named after its file*. An entry whose `header` is a **shell glob** stands for every header it matches: one class entry per file, its `name` derived from the file's stem.

```json
"classes": [
  { "header": "*.h" }
]
```

The pattern uses the same syntax as [`user_sources`](#compiling-user-sources-user_sources) — `*`, `?`, `[...]` within a path component, and a component of exactly `**` matching zero or more directories:

```json
"classes": [
  { "header": "shapes/*.h" },
  { "header": "solvers/**/*.h", "exclude": ["solvers/detail/*.h", "solvers/*_impl.h"] }
]
```

| Field | Required | Default | Meaning |
|---|:---:|---|---|
| `header` | ✅ | — | The glob. Resolved against **every** `user_include` root (a class header is emitted into `#include "..."`, so it is spelled relative to the include path — not to the manifest, as `user_sources` is), with the composed `header_dir` in front. |
| `exclude` | — | `[]` | Glob pattern, or a list of them, whose matches are dropped: the headers in the folder that declare no bound class, a `detail/` subtree, an `_impl` convention. |
| `final` | — | `false` | Applies to every matched class — it is a uniform property, unlike `name` / `expose` / `annotations` / `extensions`, which are per-class and are **rejected** on a glob entry. |

Rules:

- Only headers are taken — `.h`, `.hh`, `.hpp`, `.hxx` — so `"**/*"` cannot drag a `.cpp` or a `README` in.
- The derived name is the file's **stem**, qualified by the `namespace` in scope exactly like a spelled-out entry with no `name`. A glob works inside a group, taking that group's `header_dir` and `namespace`.
- A header whose stem is not a C++ identifier (`my-utils.h`) is **skipped with a warning** — there is nothing to derive a name from.
- Globbed entries land **after** everything spelled out and **yield to it**, by name and by header — see [The stem is the class name](#the-stem-is-the-class-name) below.

```json
"header_dir": "geom",
"namespace": "geom",
"classes": [
  { "header": "*.h", "exclude": ["fwd.h"] },
  { "header": "Mesh.h", "annotations": "Mesh.ann.json" },
  { "header": "types.h", "name": "Tolerance" }
]
```

Everything above binds: every `geom/*.h` but `geom/fwd.h`, with `geom/Mesh.h` taking its side-car and `geom/types.h` binding `geom::Tolerance` rather than a non-existent `geom::types`.

A pattern matching nothing is a **warning**, not an error (the "manifest has no class entries" check still fires if that was all there was).

### The stem is the class name

The loader never opens a bound header — it lists files. So a glob's only claim about a header is the one its **name** makes, and that claim has to hold exactly:

> **`Point.h` must declare a class named `Point`**, spelled identically (case included), in the namespace in scope for the entry (top-level `namespace`, plus any enclosing group's).

Nothing checks it at load time; a stem naming no such class surfaces as a *compile error in the generated driver* (`no member named 'vec3' in namespace 'geom'`), which is why the three cases below are worth knowing before pointing a glob at a folder.

| Your header tree | What the glob does | What to write |
|---|---|---|
| `Point.h` declares `Point` | binds `Point` | `{"header": "*.h"}` — nothing else |
| `Point.h` declares `Point` **and** `Aabb` | binds `Point` only — a file yields **one** entry | list the extra class: `{"header": "Point.h", "name": "Aabb"}` **and** `{"header": "Point.h", "name": "Point"}` (see the take-over rule below) |
| `types.h` declares `Tolerance`, `Mode` — no `types` | would bind a non-existent `types` | list them (`{"header": "types.h", "name": "Tolerance"}`, …); the take-over rule then keeps the glob off the file |
| `vec3.h` declares `Vec3` (stem ≠ class) | would bind a non-existent `vec3` | list it — `{"header": "vec3.h", "name": "Vec3"}` — or `"exclude": ["vec3.h"]` |
| `my-utils.h` (stem is no identifier) | skipped, with a warning | list it explicitly if it declares something bindable |
| `Point.h` declares `geom::detail::Point` | binds `geom::Point` — the glob applies the namespace **in scope**, it does not look inside | list it, or put the glob in a group with `"namespace": "detail"` |

**The take-over rule.** An entry that names a class of a header **speaks for that whole header**: the glob then leaves the file alone. That is what makes the `types.h` and `vec3.h` rows work — you name the classes, and no bogus stem entry appears beside them.

The cost is the second row: if the header declares *both* a class named after the file and another one you list, taking the file over drops the first. `rosetta_gen` says so rather than leaving it to be noticed —

```
rosetta_gen: note: "Point.h" is listed explicitly (class "Aabb"), so the pattern that also
matched it contributes nothing there — class "Point" is NOT bound. List it too if the header
declares both; name the header in the pattern's "exclude" if it does not (silences this)
```

— with the two fixes it names. The note fires on the `types.h` / `vec3.h` rows too, where losing the stem class is exactly what you wanted: add those headers to `exclude` to say so once and quiet it.

A name already taken is *not* reported: `{"header": "Mesh.h", "annotations": "Mesh.ann.json"}` beside a glob is the intended composition, not a loss.

Two other ways to fill `classes` from a folder, when the naming does not line up:

- `rosetta_gen --init <src_dir>` **reads** the headers (a heuristic token scan) instead of trusting their names, and writes the classes it found into a manifest — once, into a file you then edit. Use it when stems and class names disagree often.
- Keep the glob for the folder's regular part and spell out the rest; the two compose, which is the whole point of the take-over rule.

---

## Renaming a class (`expose`)

Every class binds under **one module-level name**: its `expose` override, or its unqualified C++ identifier. Two entries resolving to the same name is a `rosetta_gen` error — the module attribute and the generated trampoline (`Py_<name>` / `Js_<name>`) would collide.

That happens as soon as two bound namespaces declare the same identifier — e.g. a slip-inversion `arch::Data` next to a stress-inversion `arch::sinv::Data`. Rename one side with `expose`:

```json
"classes": [
  {"//": "stress inversion", "namespace": "sinv", "header_dir": "stress-inversion", "entries": [
      {"header": "Data.h"},
      {"header": "GpsData.h"}
  ]},
  {"//": "slip inversion", "header_dir": "slip-inversion", "entries": [
      {"header": "Data.h",    "expose": "SlipData"},
      {"header": "GpsData.h", "expose": "SlipGpsData"}
  ]}
]
```

Python then sees `arch.Data` (the `sinv` one) and `arch.SlipData`; C++ is untouched. Cross-references follow the rename automatically — a TypeScript signature touching `arch::Data` says `SlipData`, and the emitted C++ spells every bound class by its **qualified** name, so the shared identifier never becomes ambiguous in the generated TU.

`expose` is honored by **every** backend — the runtime ones (`python`, `nanobind`, `node`, `wasm`, `lua`, `julia`), the C-ABI ones (`csharp`, `java` — where it also names the generated `.cs` / `.java` file and the runtime type key), the UI inspectors (`qt`, `qml`, `imgui`), `rest` / `openapi` (route paths and schema names), and the text outputs (`typescript`, `markdown`, `html`, `json`, `paraview`). The same field works on an **enum** entry, and on a **free function** or **extension method** — see [Functions](#functions).

The rule each backend follows is the same: the exposed name is what appears in the host language (module attribute, class / enum declaration, route path, doc heading, generated file name), while every C++ spelling it emits uses the **qualified** name — which is what makes two same-named bound types unambiguous in the generated TU.

---

## Shared defaults (`namespace`, `header_dir`)

When every entry repeats the same namespace and the same header folder —

```json
"classes": [
  {"name": "stressinv::Serie",      "header": "stressinv/Serie.h"},
  {"name": "stressinv::Data",       "header": "stressinv/Data.h"},
  {"name": "stressinv::CostMetric", "header": "stressinv/cost.h"}
]
```

— factor them out with the two optional top-level defaults:

```json
"namespace": "stressinv",
"header_dir": "stressinv",
"classes": [
    {"header": "Serie.h"},
    {"header": "Data.h"},
    {"name": "CostMetric", "header": "cost.h"}
]
```

(`Serie` and `Data` also drop their `name`, since it defaults from the header stem — and the derived name is namespace-qualified too.)

Rules, applied identically to `classes`, `functions` and `extensions`:

- `namespace` qualifies an entry name only when it carries **no `::` of its own**: `"Serie"` → `stressinv::Serie`. A name containing `::` passes **verbatim** — so fully qualified spellings, nested classes (`stressinv::Model::Inner`) and sub-namespaces (`stressinv::detail::helper`) keep working unchanged, and mixing shortened and full spellings in one manifest is fine.
- A **leading `::`** pins an entry to the global namespace: `"::c_entry"` → `c_entry`. This is the escape hatch for the odd global function (e.g. `extern "C"`) in an otherwise-namespaced manifest.
- `header_dir` is prepended to **every** entry header (a `/` is inserted if missing): `"Serie.h"` → `stressinv/Serie.h`. A header living elsewhere can step out relative to it (`"../other/x.h"`), or you can keep `header_dir` unset and spell every path in full.

`rosetta_gen --init <src_dir>` factors its scanned output the same way: when every found name shares one namespace (and every header one first-level folder), the generated manifest uses these defaults instead of repeating them per entry.

### Grouped entries

One pair of top-level defaults can't cover a library whose headers live in several folders (`solvers/`, `postprocess/`, `algos/stress-inversion/`) or that uses sub-namespaces. For that, an element of `classes` or `functions` may be a **group**: an object carrying `"entries"` (a nested entry list) plus its own local defaults, instead of being an entry itself.

```json
"namespace": "arch",
"header_dir": "Arch",
"classes": [
  {"name": "Vector3", "header": "math/math.h"},

  {"header_dir": "solvers", "entries": [
    {"name": "GmresSolver", "header": "Gmres.h"},
    {"header": "ParallelSolver.h"}
  ]},

  {"header_dir": "algos", "entries": [
    {"header": "DataSuperposition.h"},
    {"namespace": "sinv", "header_dir": "stress-inversion", "entries": [
      {"header": "JointData.h"},
      {"header": "types.h", "entries": [
        {"name": "MCMCConfig"},
        {"name": "MCMCResult"}
      ]}
    ]}
  ]}
]
```

Group rules:

- A group's `header_dir` **appends below** the inherited dir: `Arch` + `solvers` ⇒ `Arch/solvers/Gmres.h`.
- A group's `namespace` **appends to** the inherited one: `arch` + `sinv` ⇒ `arch::sinv::JointData`. A leading `::` makes it absolute instead of appending.
- A group may set a `header`: the default header for entries that spell none — the natural shape for a run of classes declared by one header (`types.h` above ⇒ `arch::sinv::MCMCConfig` et al., all from `Arch/algos/stress-inversion/types.h`). Such entries need an explicit `name` (the stem fallback would give every one the same name).
- Groups **nest**, **mix freely with plain entries** in the same array, and work identically under `functions` — where a shared-header group reads especially well (a dozen shape generators all declared by `shapes.h`).
- A group cannot carry a `name`, and `//`-comment keys are ignored on groups like everywhere else.

Everything is resolved at load time, so backends and generated output are byte-identical to the fully spelled form.

Members the emitted binding could not compile are **skipped** rather than fatal: a public field whose type is a non-copyable class (e.g. a member object holding a back-reference to its owner), a method returning a reference to such a type, or a by-value parameter of one. The class still binds — as an opaque handle plus whatever members do marshal — and [extension methods (#extension-methods-extensions) fill the gaps.

---

## Extension methods (`extensions`)

Some libraries keep their real API where no binding generator can reach it: `GEO::Mesh`'s geometry lives behind public member objects with raw `double*` accessors, its I/O helpers are overloaded, its UV coordinates sit in an attribute template. Rather than hand-writing a wrapper *class*, list plain free functions — whose **first parameter is `Cls&` (or `const Cls&`)** — as `extensions` of the bound class; they appear to every backend as ordinary instance methods:

```json
"classes": [{
  "name": "GEO::Mesh", "header": "geogram/mesh/mesh.h",
  "extensions": [
    { "name": "georo::set_surface", "header": "mesh_ext.h",
      "doc": "Set vertices + triangles from flat arrays." },
    { "name": "georo::vertices",    "header": "mesh_ext.h",
      "doc": "Vertex coordinates as a flat array." }
  ]
}]
```

```py
m = geogram.Mesh()
m.set_surface(coords, triangles)   # calls georo::set_surface(m, ...)
print(len(m.vertices()) // 3)
```

The receiver is dropped from the exposed signature; the remaining parameters and the return type marshal exactly like a free function's. The method name is the function's unqualified identifier. Supported by the `python`, `nanobind`, `node`, `wasm` targets and all text backends (`typescript`, `markdown`, `html`); backends that can only emit member pointers (`qt`/`qml`/`csharp`/`java`) skip them.

---

## Pinning the runtime (`python`, `requires_python`, `napi_version`, `node_engine`)

By default a generated project takes whatever the `PATH` gives it: the emitted CMake probes `python3` / `node`, with a 3.8 floor and N-API 8 written in. Pin them per target instead:

```json
"targets": [
  { "lang": "nanobind", "name": "geom",
    "python": "3.11", "requires_python": ">=3.10" },
  { "lang": "node", "name": "geom",
    "napi_version": 9, "node_engine": ">=18" }
]
```

| field | effect | default |
| --- | --- | --- |
| `python` | The interpreter the binding is built for. A bare version (`"3.11"`) becomes `python3.11`, resolved on `PATH`; anything else is used as written (`"/opt/py311/bin/python3"`). | probe `python3` |
| `requires_python` | Minimum version. Feeds **both** `find_package(Python …)` and `pyproject.toml`'s `requires-python`, so the build floor and the wheel metadata cannot drift apart. | `>=3.8` |
| `napi_version` | `NAPI_VERSION=` for the addon. This is the real Node floor — N-API 8 means Node 12.22+, N-API 9 means Node 18.17+. | `8` |
| `node_engine` | `engines.node` in `package.json` — documentation for npm rather than a compile setting, which is why it is separate from `napi_version`. | absent |

Per-target, not top-level, so a manifest carrying both a `python` and a `nanobind` target can point them at different interpreters.

Two things worth knowing:

- **`--cmake-arg -DPython_EXECUTABLE=…` does not work**, which is why this exists. The generated CMake sets that variable with `CACHE … FORCE` after probing, overriding anything passed on the command line. Before these fields, a venv or a `pyenv` shim was the only way to choose an interpreter.
- **`python` also drives the wheel.** A wheel is tagged for whichever interpreter runs `make_wheel.py`, so `rosetta_gen --build --wheel` packages with the pinned interpreter rather than the probed one — otherwise a 3.11 build would emit a `cp312` wheel.

---

## Artifact output directory (`out_dir`)

A generated project drops its built module next to its own sources — handy for a smoke test, useless when the `.so` belongs inside your Python package or the `.js` next to a web app's assets. Name a directory and the artifact is copied there after **every** build:

```json
"targets": [
  { "lang": "nanobind", "name": "implicit3d", "out_dir": "./dist" },
  { "lang": "wasm",     "name": "implicit3d", "out_dir": "../www/assets" }
],
"out_dir": "./dist"
```

The top-level `out_dir` is the default for every target that names none; a target's own entry wins. Paths resolve against the manifest's directory and are created if missing.

This is **not** where the generated project goes (that is `rosetta_gen`'s own output tree, `--bindings-dir`) — it is where the loadable artifact lands: `libfoo.so` / `foo.pyd`, `foo.node`, `foo.so` for Lua, and for wasm **both** halves, the `.js` loader and its `.wasm`. The copy is `copy_if_different`, so an unchanged build touches nothing downstream, and the existing next-to-the-sources copy still happens.

Supported by every backend that builds a loadable module: `python`, `nanobind`, `node`, `wasm`, `lua`, `julia`. The document backends (`markdown`, `typescript`, `json`, …) have no artifact to place, and the application-shaped ones (`qt`, `qml`, `imgui`, `rest`, `csharp`, `java`) are not covered.

---

## Foreign sequence containers (`sequences`)

Many libraries carry their bulk data in their **own vector type** — geogram's `GEO::vector<T>`, an aligned or pooled vector — and the marshalling layers only know `std::vector`. List the container template (qualified, **one type parameter**) under `sequences` and it crosses the boundary like a `std::vector` of its element:

```json
"sequences": ["GEO::vector"]
```

`rosetta_gen` emits `template <typename T> struct rosetta::is_sequence<GEO::vector<T>> : std::true_type {};` into the generated `bindings.h` (equivalently, write that specialization yourself for programmatic use — see `rosetta/sequence.h`). The container must be default-constructible with `value_type`, `size()`, `resize(n)` and `begin()`/`end()`; elements must be arithmetic, `bool`, `std::string` or a bound enum.

The opted-in backends (`python`, `nanobind`, `node`, `wasm`, `lua`, plus `typescript` declarations) marshal it **by copy through a `std::vector<element>` boundary** inside an emitted adapter — scripts pass and receive plain arrays/lists/tables. Every other backend keeps skipping the type (the IR leaves `kind` "unknown", like raw pointers and callbacks). Three consequences worth knowing:

- **Mutable `Seq&` parameters bind, input-only** — the adapter's local container is a real lvalue (geogram's `assign_points(vector<double>&, dim, steal)` works; `steal` steals from the adapter's copy, which is fine). In-place mutations are discarded, exactly like pybind's `std::vector&` casters.
- **Overload sets whose sequence overload is the one that binds** — the adapter calls the method *by name* with concrete arguments instead of spelling the ambiguous `&T::name` member pointer. The walk now emits every overload, but a backend that keys methods by name binds the **first-declared** one (see [overloads and coverage](COVERAGE.md)), so `GEO::MeshVertices::assign_points` (sequence overload first) binds; a set whose first declaration is the raw-pointer one stays skipped there.
- **Virtual methods naming the container can't be overridden script-side** (their trampoline `sig_bindable` is off — the exact spelling can't round-trip), but they still bind as callable methods.

Sequence-typed public **fields** bind as copying properties (python / nanobind / wasm / lua; node skips them).

### Registering a concrete type

The string form registers a **template**, and the adapter spells the container it builds as `Namespace::Template<element>`. That composition is wrong the moment the specialization carries more than its element — `Eigen::VectorXd` is `Eigen::Matrix<double, -1, 1>`, and the composed `Eigen::Matrix<double>` does not compile. Register the concrete type instead, spelled exactly as the adapter should write it:

```json
"sequences": [{ "type": "Eigen::VectorXd" }]
```

which emits a full specialization plus the spelling (`rosetta::is_sequence<Eigen::VectorXd>` and `rosetta::sequence_cpp_name<Eigen::VectorXd>`) rather than the partial one. `{ "template": "GEO::vector" }` is the long form of a plain string entry; an object with both keys, or neither, is an error — the two cannot be told apart by looking at the text, and guessing wrong emits code that does not compile.

The container requirements are unchanged, and a real `Eigen::VectorXd` meets them (`value_type`, `size()`, `resize(n)`, `begin()`/`end()` since Eigen 3.4). This is also the only way to register a container that is not a template at all.

---

## Foreign matrices (`matrices`)

`sequences` one dimension up. A 2-D type — `Eigen::MatrixXd`, a library's own dense grid — is a sequence in no useful sense, so it needs its own registration:

```json
"matrices": [{ "type": "Eigen::MatrixXd" }]
```

Entries take the **same two forms** as `sequences`: a plain string for a one-type-parameter template (`"mylib::Grid"`, spelled `mylib::Grid<double>` by the adapter), or `{ "type": "..." }` for a concrete type spelled exactly. `rosetta_gen` emits `rosetta::is_matrix` (plus `rosetta::matrix_cpp_name` for the concrete form) into `bindings.h`; write them yourself for programmatic use — see `rosetta/matrix.h`.

The type must be default-constructible with `value_type`, `rows()`, `cols()`, `resize(r, c)` and `operator()(i, j)`; the element must be **arithmetic** (a grid of strings has no natural script shape).

The opted-in backends (`python`, `nanobind`, `node`, `wasm`, `lua`, plus `typescript` declarations, which say `number[][]`) marshal it **by copy through a `std::vector<std::vector<element>>` boundary** — an array of row arrays. Every other backend keeps skipping the type, exactly as for sequences.

Three things to know:

- **Row-major, whatever the matrix stores.** `operator()(i, j)` is the only access the registration promises, so the boundary is built row by row regardless of the type's own storage order.
- **A ragged incoming array is squared off** to the first row's length: rows are the outer size, columns the first row's, short rows keep whatever `resize()` left and extra columns are dropped.
- **A mutable `Mat&` parameter binds input-only**, like a sequence one — the adapter's local is a real lvalue, but in-place mutations are discarded.

Where an `interop` library owns the type, the same split applies: registering `Eigen::MatrixXd` here gives node / wasm / lua the array of rows and costs the Python family nothing — they keep the numpy caster. See [Foreign-library interop](#foreign-library-interop-interop).

---

## Foreign-library interop (`interop`)

A method taking or returning `Eigen::VectorXd` is, to the reflection walk, a method taking an ordinary class. Every backend used to bind it happily — and the call then **threw at run time**, because no caster for that class was ever registered. The workaround was a hand-written extension file per class flattening every Eigen type to `std::vector<double>`.

`interop` names foreign libraries whose types the target's binding framework can already marshal on its own:

```json
"interop": ["eigen"]
```

`rosetta_gen` emits `template <> struct rosetta::interop_enabled<rosetta::eigen_interop> : std::true_type {};` into the generated `bindings.h` (or write it yourself — see `rosetta/interop.h`). Recognition is by **enclosing namespace**, not by a list of type names, so one line covers everything the library owns: `VectorXd`, `MatrixXd`, `Vector3d`, `Map`, `Ref`, `Block`, `Array`, the sparse types — including spellings a hand-maintained list would have missed.

What follows splits by backend:

| Backend | Behaviour |
| --- | --- |
| `python` | Emits `#include <pybind11/eigen.h>`; the types bind **natively as numpy arrays**, both directions, matrices included. No adapter, no copy where the layout allows a view. |
| `nanobind` | Same, through `#include <nanobind/eigen/dense.h>`. (Sparse is not emitted — `<nanobind/eigen/sparse.h>` costs compile time a rare signature doesn't justify.) |
| every other backend | **Skips** the member. Deliberate, and an improvement: a skipped method is honest, a bound one that always throws is not. |

The IR marks these types with `GenType::interop` and leaves `kind` "unknown" — the same pattern raw pointers, callbacks and foreign sequences use, which is what makes the non-caster backends skip them without a single change of their own.

Two practical notes:

- **Eigen must be on the include path** of the generated binding. It already is if the manifest's `user_include` lists it, which it must anyway for the bound headers to compile.
- **The caster header is only emitted when a bound signature actually names such a type.** Declaring `interop` on a library that never exposes one costs nothing.

This replaces the flat-array extension-method pattern for the Python-family backends.

### Reaching the caster-less backends: register the type as a sequence too

`node` / `wasm` / `lua` have no caster to lean on, so on their own they skip every Eigen-typed member. Give them the flat array by ALSO registering the concrete type under [`sequences`](#registering-a-concrete-type):

```json
"interop":   ["eigen"],
"sequences": [{ "type": "Eigen::VectorXd" }]
```

The IR then carries both marks, and each backend picks:

| Backend | Behaviour |
| --- | --- |
| `python` / `nanobind` | The **caster wins** — still numpy, still no copy. A dual-marked type costs them nothing. |
| `node` / `wasm` / `lua` | Bind through the **sequence adapter**: `Eigen::VectorXd` in, out and back through a `std::vector<double>` boundary; scripts see a plain array. |
| `typescript` | Declares `number[]`, matching what those runtimes hand out. |

The sequence rules apply as written — the copy is real, and a mutable `Eigen::VectorXd&` parameter binds input-only.

**1-D only.** `MatrixXd` has no useful reading as a sequence — register it under [`matrices`](#foreign-matrices-matrices) instead, which splits the backends exactly the same way:

```json
"interop":   ["eigen"],
"sequences": [{ "type": "Eigen::VectorXd" }],
"matrices":  [{ "type": "Eigen::MatrixXd" }]
```

covers both shapes, in both directions, on every expanded backend.

---

## Standard types that need no declaration: `std::filesystem::path`, `std::shared_ptr<T>`

Nothing to write in the manifest for these two — they are listed here because
until recently a signature naming either was quietly unbindable on the
caster-less backends, and a factory API tends to name both at once:

```cpp
std::shared_ptr<Mesh> compile_file(const std::filesystem::path &input);
```

**`std::filesystem::path` marshals as a string.** To the walk it used to be an
ordinary unregistered class, so every member naming one was skipped everywhere.
It is now described as `kind: "string"` with an `is_path` flag, and the two
families differ in how they honour it:

| Backend | Behaviour |
| --- | --- |
| `python` / `nanobind` | **Native**, through `<pybind11/stl/filesystem.h>` / `<nanobind/stl/filesystem.h>` — callers pass a `str` **or** any `os.PathLike`, and a returned path comes back as `pathlib.Path`. |
| `node` / `wasm` / `lua` | Through the same **copy adapter** the foreign containers use: the boundary declares `std::string`, the emitted code builds a `std::filesystem::path` from it and calls `.string()` on the way back. Scripts see a plain string. |
| `typescript` | Declares `string`. |

**`std::shared_ptr<T>` crosses in the return direction**, which is where a
factory needs it. The pointee `T` must itself be a bound class; what each
backend does with it:

| Backend | Behaviour |
| --- | --- |
| `python` | `py::class_<T, std::shared_ptr<T>>` — the holder is declared automatically for every pointee the module hands out. |
| `nanobind` | Native, through `<nanobind/stl/shared_ptr.h>`. |
| `wasm` | The class registration gains `.smart_ptr<std::shared_ptr<T>>("T_sp")`, and only for classes that actually travel that way. |
| `lua` | Native — sol2's own `unique_usertype_traits` handles it; the userdata is T's usertype. |
| `node` | The JS object **adopts** the pointer: a third ownership mode in the wrapper, next to "owns" and "aliases a member of a pinned parent". The C++ object lives as long as the JS handle does. |
| `typescript` | Declares the **pointee** (`Doc`), not `shared_ptr`. |

Two limits worth knowing:

- **Returns only.** A `shared_ptr<T>` *parameter* stays skipped: converting an
  incoming script object would mean manufacturing a control block over memory
  the binding does not own — a double-free waiting to happen. Take `const T&`.
- **node: untrampolined pointees only.** A class with virtual methods is wrapped
  as its trampoline subclass, and the adopting wrapper stores a `T*`; reading
  one as the other is undefined, so such a return stays skipped there (the other
  backends have no such restriction).

Both work with a non-copyable `T` — which is the point, since a class handed out
by a factory usually is one.

---

## Doc comments (`doc_comments`)

Your library is probably already documented. `rosetta_gen` reads that
documentation out of the headers it is already opening and hands it to every
backend, so a generated binding is not born blank.

```json
{ "doc_comments": false }     // default: true
```

Given this header — which mentions rosetta nowhere:

```cpp
/// A circle in the plane.
struct Circle {
    /// Distance from the centre to the rim.
    double radius = 1.0;

    /**
     * @brief Approximate the outline as a polygon.
     * @param segments  how many straight edges to use
     * @param close     whether to repeat the first point at the end
     * @return the vertex coordinates, x and y interleaved
     */
    std::vector<double> outline(int segments = 32, bool close = true) const;
};
```

the Python binding becomes

```python
>>> help(shapes.Circle.outline)
outline(self, segments: int = 32, close: bool = True) -> list[float]

    Approximate the outline as a polygon.

    Args:
        segments: how many straight edges to use
        close: whether to repeat the first point at the end

    Returns:
        the vertex coordinates, x and y interleaved

>>> c.outline(segments=8, close=False)      # keyword arguments, C++ defaults
```

and the TypeScript declaration becomes

```ts
/**
 * Approximate the outline as a polygon.
 * @param segments how many straight edges to use
 * @param close whether to repeat the first point at the end
 * @returns the vertex coordinates, x and y interleaved
 */
outline(segments?: number, close?: boolean): number[];
```

### What it reads

| From the header | Where it lands |
|---|---|
| `///`, `//!`, `/** */`, `/*! */` blocks, and `///<` trailing comments | the member's doc / docstring / TSDoc / Javadoc / XML doc / OpenAPI `description` |
| `@brief`, `@details`, untagged prose | the same |
| `@param <name> …` (with `[in]` / `[out]` / `[in,out]` markers) | `GenParam::doc` — a `py::arg` name plus an `Args:` section, a TSDoc `@param`, an OpenAPI parameter description |
| `@return` / `@returns` | `GenMethod::returns` — a `Returns:` section, a TSDoc `@returns` |
| `@note`, `@warning`, `@deprecated`, `@throws`, `@pre`, `@post`, `@see`, `@since` | labelled paragraphs appended to the doc |
| a class's own comment | `GenClass::brief` — the Python type's docstring, the TypeScript class comment |
| **default arguments** (`int segments = 32`) | `GenParam::default_text` — a real `py::arg("segments") = 32`, an optional `segments?` in TypeScript |

`@tparam`, `@file`, `@ingroup` and the other bookkeeping commands are dropped.

### Why default arguments come from the text

Reflection reports *that* a parameter has a default (`std::meta::
has_default_argument`, surfaced as `GenParam::has_default`) but not what the
expression was — P2996 has no way to ask. So the fact and the spelling arrive by
different routes and are **cross-checked**: a spelling is used only where
reflection independently confirms a default exists. A parameter known to be
optional whose default could not be recovered is still marked optional; it just
has no value to show.

The harvester is deliberately conservative about which defaults it will repeat
into generated C++, because a wrong one does not produce a bad docstring — it
produces a binding that does not compile. It accepts literals (`32`, `1e-6`,
`true`, `"auto"`, `{}`) and qualified-ids (`Mode::Fast`); it refuses anything
with a call, an operator or a comma in it (`Point(0, 0)`, `a + b`,
`sizeof(int)`), and it refuses an **unqualified** enumerator (`Fast`), which
would not resolve where the binding spells it.

The Python family adds one more rule of its own: pybind converts an `arg`
default into a Python object at **module import**, so a default whose type is
not registered fails the import outright. Defaults are therefore emitted for
numbers, booleans, strings, and enums bound in the same module — never for a
class type or an unbound enum.

### Precedence

Harvested text only ever **fills a blank**. In order:

1. an inline `[[= rosetta::doc{"…"}]]` annotation (the harvester skips a member that carries one);
2. a JSON side-car entry (see [out-of-line annotations](OUT_OF_LINE_ANNOTATIONS.md)), or a manifest `doc` on a free function;
3. the header's own comment.

So adopting `doc_comments` cannot change a binding you have already annotated,
and the two mechanisms compose: annotate the handful of members that need
`range`, `readonly` or a rewritten description, and let the headers document
the rest.

A complete, runnable walkthrough — including the cases where the harvester
deliberately does less than you might expect — is in
[`examples/doxygen`](../examples/doxygen).

### What it will not do

It is a scanner, not a compiler: it does not expand macros or follow
`#include`. Everything it records is matched against the reflected signature
before it is used — by arity and by parameter names — and dropped when the two
disagree, so a mis-read declaration loses its documentation rather than
attaching it to the wrong member. When two overloads of a name are
indistinguishable, **neither** is documented. Private members are never read;
neither are constructors, destructors or operators, none of which have a doc
slot in the IR. Members inherited from a base declared in a header no class
entry names are not covered — list that base as a class to reach it.

---

## Out-parameters (`out_params`)

```cpp
bool get_doubles(const std::string &name, vector<double> &out, index_t &dim) const;
```

Three results, C++-style. No host language has that shape, and every backend
skipped such a member: the argument its runtime converts is a temporary, which
cannot bind to a non-const reference. Name the outputs and they leave the
exposed signature and join the return instead:

```json
"out_params": {
  "AttributesManager::get_doubles": [1, 2]
}
```

Keys are `"Class::method"` or `"ns::function"`; values are **0-based parameter
indices**. A key matching nothing is reported on stderr rather than ignored.

```py
ok, uv, dim = mesh.facet_corners.attributes().get_doubles("tex_coord")
```

| Backend | Shape |
| --- | --- |
| `python` / `nanobind` | A tuple — `(ok, uv, dim)`. |
| `lua` | Lua's own **multiple returns** — `local ok, uv, dim = …`. |
| `node` | An **array** — `const [ok, uv, dim] = …`. |
| `wasm` | An array too, built as an `emscripten::val` (embind marshals no tuple). |
| `typescript` | A tuple type: `get_doubles(arg0: string): [boolean, number[], number]`. |

The return value comes first when the function has one; with a `void` return
the outputs are all there is. A foreign container out-parameter arrives already
flattened to the boundary array, exactly as a container return does.

**Never inferred, and that is the whole design.** These two are the same C++:

```cpp
void assign_points(vector<double> &pts, index_t dim, bool steal); // an INPUT it steals from
bool get_doubles(const std::string &, vector<double> &out, index_t &dim); // an OUTPUT
```

Guessing wrong silently drops an argument in one direction or a result in the
other, so rosetta does not guess: an unmarked mutable reference keeps binding
exactly as before (input-only for a container, skipped for a scalar). Marking a
`const` reference does nothing — the callee cannot write through it. A mutable
reference to a **bound class** is not an out-parameter either: it already
crosses as a handle the callee writes through, which is the more faithful
reading and needs no adapter.

---

## Module init (`module_init`)

A library is more than its API. geogram wants `GEO::initialize()`, a run of
`CmdLine::import_arg_group()` calls and a C-function-pointer registration to
have happened before anything works — none of which is expressible as "bind
this name", so it lived in a hand-written `georo::initialize()` that every
script had to remember to call first. `module_init` names statements the
generated module runs **when it loads**:

```json
"module_init": {
  "headers": ["geogram/basic/command_line.h", "geogram/basic/command_line_args.h"],
  "statements": [
    "GEO::initialize(GEO::GEOGRAM_INSTALL_NONE)",
    "GEO::CmdLine::import_arg_group(\"standard\")",
    "GEO::CmdLine::import_arg_group(\"algo\")"
  ]
}
```

A bare array is the shorthand for `statements` alone. The statements are spliced
**verbatim** into the module entry point — `PYBIND11_MODULE` / `NB_MODULE` /
`Init` / `EMSCRIPTEN_BINDINGS` / `luaopen_*` / `JLCXX_MODULE` — at the very top,
before any binding is registered, so an I/O handler an init call installs is in
place before a bound loader can be reached. A trailing `;` is optional (and never
doubled). `headers` are emitted as `#include` lines ahead of the bound classes'
own, since an init statement usually names a namespace the bound headers never
mention.

Because the statements are spliced verbatim, they must compile at that point:
spell types qualified, and remember the module has no arguments to offer — this
is *fixed* start-up work, not a configurable entry point. Anything a caller
should be able to vary (a verbosity flag, a thread count) still belongs in a
bound function.

Emitted by the `python`, `nanobind`, `node`, `wasm`, `lua` and `julia`
backends — the ones with a module entry point. The C#, Java,
REST, Qt/QML/ImGui and documentation backends ignore the field: they have no
single load-time hook to hang it on.

---

## Generated headers (`generated_headers`)

A header the bound library's own build system produces, which is simply absent
when rosetta compiles that library's sources without running its build. geogram's
`<geogram/version.h>` is the case in point — configured by geogram's CMake from
`version.h.in`, and previously faked by a checked-in copy that shadowed the real
one:

```json
"generated_headers": [
  {
    "path": "geogram/version.h",
    "template": "./extern/geogram/src/lib/geogram/basic/version.h.in",
    "substitutions": {
      "VORPALINE_VERSION_MAJOR": "1",
      "VORPALINE_VERSION_MINOR": "10",
      "VORPALINE_VERSION_PATCH": "1",
      "VORPALINE_VERSION": "1.10.1-rosetta",
      "VORPALINE_BUILD_NUMBER": "",
      "VORPALINE_BUILD_DATE": "",
      "VORPALINE_SVN_REVISION": ""
    }
  }
]
```

| Field | Required | Meaning |
|---|:---:|---|
| `path` | ✅ | The **relative include path** — exactly what the sources `#include`. |
| `template` | one of | A file with `@KEY@` placeholders (CMake's `configure_file` form), resolved against the manifest's directory. |
| `content` | one of | Literal lines, as an array of strings, for a header with no template. |
| `substitutions` | — | `@KEY@` → value, applied to `template`. |

The file is written to `<bindings>/include/<path>` and that directory is placed
**first** on every generated target's include path — ahead of the library's own
sources, so a stale copy of the same header cannot win the lookup. Substitution
happens when the manifest is read, so a missing key is reported against your
manifest (`no substitution for @VORPALINE_VERSION@`) rather than compiling a
literal `@KEY@` into the binding; only the `@KEY@` form is recognised, since
`${KEY}` would collide with the shell and CMake expansions such templates carry.

A `-D` define is a different tool for a nearby job: geogram's `GEOGRAM_VERSION`
is passed by its CMake on the command line, not written into the header, so it
belongs in [`compile_definitions`](#preprocessor-definitions-compile_definitions).

---

## Multiple include directories

When your headers don't all live under a single root, give `user_include` an **array** of directories instead of a single string:

```json
"user_include": ["./geom", "../shared/include", "/opt/thirdparty/include"]
```

Each entry follows the same resolution rules as the single-string form (relative to the manifest, or absolute). Every directory is added to the generated bindings' include path, so a class `header` is resolved against **all** of them — the first directory that contains the file wins, exactly like a compiler's `-I` search order. The array must not be empty.

The single-string form is just the one-directory shorthand:

```json
"user_include": "./geom"          // equivalent to ["./geom"]
```

---

## Functions

`functions` binds **free (non-member)** functions without editing your headers. Each entry:

```json
"functions": [
  { "name": "transform", "header": "common.h",
    "doc": "Scale and swizzle a point into (x*2, z*3, y*4)" }
]
```

| Field | Required | Default | Meaning |
|---|:---:|---|---|
| `name` | ✅ | — | Function name. May be namespace-qualified (`api::add`). |
| `header` | ✅ | — | Header declaring it (emitted into `#include`). |
| `doc` | — | — | Optional description (free functions carry no in-source annotations). |
| `expose` | — | unqualified `name` | The **binding name** — what scripts call it. Same rule as a class's [`expose`](#renaming-a-class-expose): a plain identifier, leaving the emitted C++ (which spells the function qualified) untouched. |
| `signature` | — | — | The C++ **function type** of the one overload to bind (`"void(Mesh&, bool)"`). Required when `name` is overloaded, meaningless otherwise. See [below](#binding-one-overload-signature). |

Free functions share the module namespace with classes, so `arch::solve` and `arch::sinv::solve` — or a function and a class of the same name — collide exactly like two classes do, and `rosetta_gen` rejects the manifest with the same "rename one with `expose`" error:

```json
"functions": [
  { "name": "arch::solve",       "header": "slip.h" },
  { "name": "arch::sinv::solve", "header": "stress.h", "expose": "solve_stress" }
]
```

Every backend that binds free functions honors it (python, nanobind, node, wasm, lua, julia, C#, Java, REST, typescript, markdown, html, openapi) — each emits the exposed name as a label and the qualified name as the C++ spelling.

`expose` works the same way on an [extension method](#extension-methods-extensions), where it renames the method on the class it attaches to (and two extensions of one class may not resolve to the same name).

### Binding one overload (`signature`)

An overloaded free function has no reflection to name: `^^GEO::mesh_union` is
ill-formed the moment the namespace declares the name twice, so the entry could
not be written at all — the workaround was a wrapper function under a different
name, in a file you then had to maintain. Give the entry the **signature** of the
one you want instead:

```json
"functions": [
  { "name": "GEO::mesh_union", "header": "geogram/mesh/mesh_surface_intersection.h",
    "signature": "void(GEO::Mesh&, const GEO::Mesh&, const GEO::Mesh&, bool)" }
]
```

The signature is a C++ function type — return type first, then the parameter
list — and it is spliced into the generated driver **verbatim**, so it must
resolve there: spell the types the way the header does, qualified. rosetta_gen
checks only its shape (a return type, balanced parentheses); the types are the
compiler's business, and an error names your manifest entry's own spelling.

It is not a *filter* over the overload set: it names one member, and only that
one binds. List the entry twice, with two signatures and two `expose` names, to
bind two of them:

```json
{ "name": "GEO::mesh_union", "header": "…", "expose": "mesh_union_flags",
  "signature": "void(GEO::Mesh&, const GEO::Mesh&, const GEO::Mesh&, GEO::MeshBooleanOperationFlags)" }
```

Backends form the function pointer through a disambiguating
`static_cast<void(*)(…)>(&GEO::mesh_union)` — including where the pointer is a
template argument (`napi_free_entry<…>`) — so **every backend that emits a
pointer binds the selected overload**: python, nanobind, wasm, lua,
node, julia, C#, Java, and typescript declares it like any other function. The
one that spells the function's *reflection* instead — `rest`, which emits
`bind_rest_function<^^name>` — **skips the entry with a note on stderr**: there
is no reflection for one member of an overload set, and a skipped function is
honest where an ambiguous one would not compile.

Overload selection is a `functions` feature. An [extension
method](#extension-methods-extensions) cannot take a `signature` (rosetta_gen
rejects it): an extension reaches the backends as a *method*, whose emitters
spell its address in many more places than a free function's single
address-of.

See [FREE_FUNCTIONS](FREE_FUNCTIONS.md) for details.

---

## Targets

`targets` lists the language backends. Each entry is **either**:

- a **bare string** — uses `module_name` (or `generator_name`) as the module name:

  ```json
  "targets": ["python", "node", "markdown"]
  ```

- an **object** `{ "lang": ..., "name": ... }` — sets a per-target module name:

  ```json
  "targets": [
    { "lang": "python", "name": "pygeom" },
    { "lang": "node",   "name": "jsgeom" },
    { "lang": "markdown" }
  ]
  ```

`name` is optional in the object form too (defaults to `module_name`). One generator emits a **single combined module per target** exposing every class.

The object form also accepts **`link_options`** — extra linker flags applied to *this target only*, emitted as `target_link_options(... PRIVATE ...)` in the generated project. Per-target (unlike `compile_definitions`) because link flags are toolchain-specific — e.g. geogram's `GEO::initialize()` mounts the host filesystem with NODEFS under Node, which needs emscripten's nodefs library on the **wasm** link line and would break a native link:

```json
"targets": [
  { "lang": "python" },
  { "lang": "wasm", "link_options": ["-lnodefs.js"] }
]
```

The flags are emitted **after** the template's own `target_link_options`, so on the wasm targets — where emcc honors the *last* occurrence of a repeated `-s` setting — they can also **override** a template default, e.g. `"-sALLOW_MEMORY_GROWTH=0"` for a fixed-size heap.

### Available `lang` values

Every compiled target is **expanded**: reflection runs once on the generation host and the emitted code builds with an off-the-shelf compiler. `rest` and `json` are the two exceptions — both emit a project whose sources include a *runtime visitor* (`rosetta/visitors/rest.h`, `rosetta/visitors/json.h`) that still splices reflections, so those two need the C++26 toolchain to build.

| `lang` | Output | Builds with |
|---|---|---|
| `python` | pybind11 extension module | off-the-shelf compiler |
| `nanobind` | nanobind extension module | off-the-shelf compiler |
| `node` | N-API native addon | off-the-shelf compiler |
| `wasm` | Emscripten / embind module | off-the-shelf emsdk |
| `qt` | Qt Widgets property/method inspector | off-the-shelf compiler + Qt 6 |
| `imgui` | Dear ImGui inspector app (GLFW + OpenGL3, auto-fetched) | off-the-shelf compiler |
| `qml` | QML / QtQuick inspector | off-the-shelf compiler + Qt 6 |
| `csharp` | C-ABI shared lib + P/Invoke wrappers | off-the-shelf compiler + .NET |
| `java` | C-ABI + handle-backed FFM wrappers | off-the-shelf compiler + JDK |
| `julia` | CxxWrap.jl / jlcxx shared module | off-the-shelf compiler + CxxWrap.jl |
| `lua` | sol2 shared module, `require`-able (Lua 5.1–5.4 / LuaJIT) | off-the-shelf compiler |
| `rest` | cpp-httplib JSON server + browser client | **C++26 toolchain** |
| `openapi` | OpenAPI 3.1 spec | text output |
| `json` | nlohmann (de)serialization + `demo.cpp` | **C++26 toolchain** |
| `typescript` | ambient `.d.ts` declarations | text output |
| `markdown` | API reference document | text output |
| `html` | styled API reference page | text output |
| `paraview` | ParaView Server Manager plugin XML | text output |

Each name also accepts a deprecated `-expanded` spelling (`lua-expanded`, `qt-expanded`, …) resolving to the same backend, so manifests written before the suffix was dropped keep working.

The text-only outputs (`markdown`, `html`, `typescript`, `openapi`, `paraview`) don't compile anything, so the C++26-vs-off-the-shelf distinction doesn't apply — they're produced directly. `json` is *not* one of them: besides the serialization header it emits a buildable `demo.cpp` + `CMakeLists.txt` at `CMAKE_CXX_STANDARD 26`.

> **Why expanded?** Your *target* compiler doesn't have to support reflection: generate once on a C++26 / P2996 host, then ship and build the generated sources anywhere with an off-the-shelf toolchain (plain Clang / GCC / MSVC, off-the-shelf emsdk, off-the-shelf Qt 6). The generator host still needs C++26; the target does not. Pairs naturally with out-of-line annotations so the bound headers stay plain C++ too. See [`examples/geom-expanded`](../examples/geom-expanded).

---

## Linking external libraries (`user_lib`)

Use `user_lib` when your bound headers only **declare** the API and the definitions live in a separately-compiled library (`.so` / `.dylib` / `.a`). rosetta links the generated bindings against it and sets up rpath.

```json
"user_lib": {
  "name": "space",
  "dir":  "../space/bin",
  "link": "shared"
}
```

| Field | Required | Default | Meaning |
|---|:---:|---|---|
| `name` | ✅ | — | Library name (the `space` in `libspace.dylib`). |
| `dir` | ✅ | — | Directory holding the library (relative to the manifest; used for `-L` / rpath). |
| `link` | — | `"shared"` | `"shared"` (default), `"static"`, or `"dynamic"` (alias of `"shared"`). The *preferred* form, with fallback to whichever is actually built. |

`wasm` targets are **always** static — a native `.dylib` / `.so` cannot enter a wasm module. The native `python` / `node` targets honor `link`. See [`examples/dynamic-lib`](../examples/dynamic-lib).

### Several libraries: your library and its dependencies

A bound library rarely stands alone — it links against pre-built third parties of its own. Give `user_lib` an **array** of the same objects, one per library, and every one of them is linked (and rpath'ed) into the bindings:

```json
"user_include": ["../mylib/include", "/opt/foo/include"],
"user_lib": [
  { "name": "mylib", "dir": "../mylib/bin", "link": "shared" },
  { "name": "foo",   "dir": "/opt/foo/lib" },
  { "name": "bar",   "dir": "/opt/foo/lib", "link": "static" }
]
```

- **Order is the link order** — list a library before the ones it depends on, which is what static archives require.
- Each entry keeps its own `link`, so a shared library and a static dependency mix freely.
- Every distinct `dir` lands in the binding's `BUILD_RPATH` / `INSTALL_RPATH`, so the module loads without `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`.
- The **generator driver** links the same list — it instantiates the bound types during the reflection walk, so it needs the dependencies' definitions too, not just your library's.
- Headers of the dependencies go in [`user_include`](#multiple-include-directories) (an array), their configuration macros in [`compile_definitions`](#preprocessor-definitions-compile_definitions).
- On `wasm` every entry is taken as the static archive `lib<name>.a` built with the **same** emsdk — a native `.dylib` / `.so` cannot enter a wasm module, so a dependency you cannot rebuild for emscripten rules that target out.

The single-object form is just the one-library shorthand:

```json
"user_lib": { "name": "space", "dir": "../space/bin" }
```

Flags that aren't a `-L`/`-l` pair (a framework, `--start-group`, a package's own link line) still go through a target's [`link_options`](#targets); the two compose.

---

## Compiling user sources (`user_sources`)

Use `user_sources` when your bound headers only **declare** the API and the definitions live in `.cpp` files you want **compiled straight into the binding** — rather than linked from a pre-built [`user_lib`](#linking-external-libraries-user_lib).

```json
"user_sources": [
  "../src/widget.cpp",
  "../src/shape.cpp"
]
```

It is always a **list of paths**, each relative to the manifest (or absolute). Every compiled backend adds them to its binding target via `target_sources(...)`, so they build with the same include path and flags as the generated binding. A single string is accepted as a one-element list.

Each entry may be a **shell glob**, expanded at generation time:

```json
"user_sources": ["./extern/pmp/src/pmp/algorithms/*.cpp"]
```

`*`, `?` and `[...]` are supported within a path component, and a component that is exactly `**` matches **zero or more directories** (bash-globstar style) — so one pattern covers a whole source tree instead of one line per subdirectory:

```json
"user_sources": ["extern/arch/src/**/*.cxx"]
```

matches `src/a.cxx` as well as `src/algos/stress-inversion/b.cxx`. Like the other wildcards, `**` does not enter hidden (dot) directories. Matches are sorted for reproducible output, and the final list is de-duplicated, so mixing a literal path with a glob that also covers it is safe. A pattern that matches nothing emits a warning and is skipped. Because globs expand when `rosetta_gen` runs, **re-run it after adding or removing matching files**.

`user_sources` and `user_lib` are independent — use either, or both. The text-only backends (`markdown`, `html`, `json`, `typescript`, `openapi`, `paraview`) compile nothing and ignore it.

Entries may also be **C sources** (`.c`) — e.g. a third-party library's vendored dependencies (zlib, rply, libMeshb, OpenNL…). When any `.c` file is listed, the generated CMakeLists calls `enable_language(C)` automatically so they compile alongside the C++ binding:

```json
"user_sources": [
  "./geogram/src/lib/geogram/mesh/*.cpp",
  "./geogram/src/lib/geogram/third_party/rply/*.c",
  "./geogram/src/lib/geogram/third_party/zlib/*.c"
]
```

---

## Preprocessor definitions (`compile_definitions`)

Use `compile_definitions` to pass preprocessor switches to the build — most commonly a third-party library's configuration macros. Each entry is `"NAME"` or `"NAME=VALUE"`:

```json
"compile_definitions": [
  "GEOGRAM_USE_BUILTIN_DEPS",
  "GEOGRAM_WITH_HLBFGS"
]
```

(This geogram example selects the vendored libMeshb / rply / zlib over system libraries, and compiles the HLBFGS optimizer in — required by the Newton iterations of CVT remeshing.)

The definitions are emitted as `target_compile_definitions(... PRIVATE ...)` in **two** places, so the whole pipeline sees a consistent configuration:

- the **generator driver** — it includes the bound headers for the reflection walk, which must see the same preprocessor state the bindings will be built with;
- **every compiled binding target** — where they reach the bound headers and the [`user_sources`](#compiling-user-sources-user_sources) alike.

A single string is accepted as a one-element list. The text-only backends (`markdown`, `html`, `json`, `typescript`, `openapi`, `paraview`) compile nothing and ignore it.

---

## Build type & optimization (`build_type`, `optimization`)

Every compiled backend's generated `CMakeLists.txt` can carry a build configuration, so `cmake -S . -B build && cmake --build build` (and `rosetta_gen --build`) produces optimized or debuggable bindings without editing the output:

```json
"build_type": "Release",
"optimization": "-O2"
```

Both are optional and independent:

- **`build_type`** — one of `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` (case-insensitive). Emitted as a *default* inside `if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)`, so `-DCMAKE_BUILD_TYPE=...` at configure time still wins and multi-config generators (Visual Studio, Xcode) are left to pick a configuration at build time. **Omitted ⇒ `Release`.** It previously meant "emit nothing", which left CMake with no build type and hence no optimization flags at all, so the documented two-line build produced an unoptimized module — around 8× slower on a trivial pybind11 call. Pass `-DCMAKE_BUILD_TYPE=Debug` (or set `"build_type": "Debug"`) when you want an unoptimized build.
- **`optimization`** — an explicit optimization level: `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Oz`, `-Og` or `-Ofast` (the leading `-` may be omitted). Emitted as `add_compile_options(...)` / `add_link_options(...)`, which land *after* the build type's own per-config flags on the compiler command line — so this `-O` overrides the level the build type implies (e.g. `"build_type": "Release", "optimization": "-O2"` builds `-DNDEBUG` but at `-O2` instead of Release's `-O3`). The link option matters for the wasm targets, where emscripten optimizes at link time too.

The flags apply to the *bindings* (and any [`user_sources`](#compiling-user-sources-user_sources) compiled into them), in every compiled backend. The text-only backends compile nothing and ignore both.

---

## Python wheels (`version`)

The `python` and `nanobind` backends emit, alongside `CMakeLists.txt`, everything needed to build a redistributable Python wheel:

| File | Role |
| --- | --- |
| `pyproject.toml` | [scikit-build-core](https://scikit-build-core.readthedocs.io/) build config. It drives the generated `CMakeLists.txt` itself, so the wheel and a plain `cmake --build` compile the same target the same way — there is no second build description to keep in sync. |
| `make_wheel.py` | One-command builder: builds the wheel, then bundles external shared libraries and fixes the platform tag. Python rather than a shell script, so Linux, macOS and Windows are all covered by one file — building a wheel needs an interpreter anyway. |

```sh
cd bindings/nanobind
python make_wheel.py                    # -> dist/*.whl, dist/repaired/*.whl
python3.11 make_wheel.py                # a specific interpreter
python make_wheel.py --outdir /tmp/whl  # somewhere other than ./dist
python make_wheel.py --no-repair        # skip the bundling / retagging step
```

The wheel is built for whichever interpreter runs the script — there is no `--python` flag, just invoke the one you want.

`rosetta_gen --build manifest.json --wheel` runs the same step for every `python` / `nanobind` target in one go, and `--wheel-dir DIR` collects the results in one directory instead of a per-backend `dist/` (see [ROSETTA_GEN.md](ROSETTA_GEN.md#case-8-python-wheels)).

For a project that always ships wheels, say so in the manifest instead of on every command line — on the **target**, beside the interpreter it is built for:

```json
"targets": [
  { "lang": "python",   "name": "pygeom", "python": "3.11" },
  { "lang": "nanobind", "name": "nbgeom", "wheel": true, "wheel_dir": "./dist" }
]
```

| Field | Meaning |
|---|---|
| `wheel` | Build a wheel for **this** target on every `--build`, without passing `--wheel`. |
| `wheel_dir` | Where this target's wheels go instead of its own `dist/`. Implies `wheel`, and resolves against the manifest's directory. |

Both are per-target, and only the two backends that emit a `make_wheel.py` accept them — `wheel` on a `markdown` or `node` target is an error, not a no-op. This is the same shape as [`out_dir`](#artifact-output-directory-out_dir), and for the same reason: the two Python backends genuinely differ here. `nanobind` produces one `abi3` wheel covering every CPython 3.12+ where `python` produces one per version, and [`python`](#pinning-the-runtime-python-requires_python-napi_version-node_engine) already pins per target which interpreter each is tagged for. Sharing one `wheel_dir` between them is safe — each `make_wheel.py` repairs only the wheels it just produced.

The **flags still win, in the direction of doing more**. `--wheel` packages every `python` / `nanobind` target whatever the manifest says, so a target spelling `"wheel": false` cannot turn off a run that asked for it; `--wheel-dir DIR` overrides every target's own directory, which is what makes "collect them all in one place" a single flag. With neither set anywhere, `--build` only compiles the extension module.

> **Changed:** `wheel` and `wheel_dir` were **top-level** keys. They are per-target now, and a top-level one is a load error naming the fix rather than a key that silently does nothing — a manifest that used to ship wheels must not quietly stop.

`version` is the distribution version written into `pyproject.toml`:

```json
"version": "1.2.0"
```

A PEP 440 release string starting with a digit (`"1.2.0"`, `"0.3.0rc1"`; a bare number is accepted and stringified). Omitted ⇒ `0.1.0`. Edit it here and re-run the generator rather than editing the generated `pyproject.toml`, which is overwritten.

A few things worth knowing:

- **ABI tagging.** `nanobind` builds against CPython's stable ABI on 3.12+ and tags the wheel `abi3`, so one artifact covers every later interpreter. Below 3.12, and for `python` in every case (pybind11 has no stable-ABI mode), the wheel is tagged for the exact CPython that built it — covering several versions means running the script once per interpreter. The plain `cmake` build is unaffected either way; stable-ABI mode is a wheel-only switch (`-DROSETTA_STABLE_ABI=ON` forces it by hand).
- **`user_lib` and wheel repair.** The generated CMake links [`user_lib`](#linking-external-libraries-user_lib) entries by absolute path, so a freshly built module refers to a directory that does not exist on the installing machine. `make_wheel.py` runs the platform's repair tool — `delocate` (macOS), `auditwheel` (Linux), `delvewheel` (Windows) — to copy those libraries *into* the wheel and rewrite the load paths; results land in `dist/repaired`. On Linux this also retags the wheel `manylinux_*`, without which PyPI rejects it. On Windows the `user_lib` directories are baked into the script as `USER_LIB_DIRS` and handed to `delvewheel --add-path`: a `.pyd` records no search path for its DLLs (Windows has no rpath), so unlike the other two tools delvewheel cannot discover them by following a load command. Repair failing is a warning, not an error — a wheel with nothing external to bundle is already correct. Declaring `"link": "static"` sidesteps the whole question.
- **Wheels are redistributable; sdists are not.** The generated `CMakeLists.txt` embeds the header and library paths the manifest resolved on the generating machine. Ship wheels, or re-run the generator wherever you build.
- **Matrix builds.** For several Python versions and platforms in one go, use [cibuildwheel](https://cibuildwheel.pypa.io/) (`pipx run cibuildwheel --platform auto`) instead of looping over the script. This is where the `-expanded` backends pay off: the generated source needs no reflection toolchain, so off-the-shelf CI runners — including Windows/MSVC — can build it.

---

## Full reference example

```json
{
  "//1": "Bindings for the geom library, mixing per-target module names,",
  "//2": "out-of-line annotations and a free function.",

  "user_include": "./geom",
  "rosetta_include": "../../include",
  "generator_name": "geom_gen",
  "module_name": "geom",

  "//cpp26": "C++26 / P2996 reflection toolchain used to build the generator (and the rest target).",
  "cpp26_root": "$ENV{HOME}/devs/c++/clang-p2996/build",
  "cpp26_cxx":  "$ENV{HOME}/devs/c++/clang-p2996/build/bin/clang++",
  "cpp26_cc":   "$ENV{HOME}/devs/c++/clang-p2996/build/bin/clang",
  "cpp26_lib":  "$ENV{HOME}/devs/c++/clang-p2996/build/lib",

  "//build": "Default build type + explicit -O level for every compiled backend.",
  "build_type": "Release",
  "optimization": "-O2",

  "//version": "Stamped into the pyproject.toml emitted for wheel builds.",
  "version": "1.2.0",

  "targets": [
    { "lang": "python", "name": "geom" },
    { "lang": "nanobind", "name": "geom" },
    { "lang": "node", "name": "geom" },
    { "lang": "wasm", "name": "geom" },
    { "lang": "typescript" },
    { "lang": "markdown" },
    { "lang": "html" }
  ],

  "classes": [
    { "doc": "the top-level model", "name": "Model", "header": "Model.h", "annotations": "Model.ann.json" },
    { "header": "Point.h" },
    { "header": "Surface.h" },
    { "header": "Triangle.h" },
    { "header": "Kind.h" }
  ],

  "functions": [
    { "doc": "Scale and swizzle a point into (x*2, z*3, y*4)",
      "header": "common.h", "name": "transform" }
  ]
}
```

The `cpp26_*` fields point at the **C++26 / P2996 reflection compiler** used to build the generator. They are all optional — omit them and rosetta uses these defaults:

| Field | Default |
|---|---|
| `cpp26_root` | `$ENV{HOME}/devs/c++/clang-p2996/build` |
| `cpp26_cxx` | `${cpp26_root}/bin/clang++` |
| `cpp26_cc` | `${cpp26_root}/bin/clang` |
| `cpp26_lib` | `${cpp26_root}/lib` |

If your [Bloomberg `clang-p2996`](https://github.com/bloomberg/clang-p2996) build lives elsewhere, set `cpp26_root` alone — `cpp26_cxx` / `cpp26_cc` / `cpp26_lib` all derive from it. Override the individual ones only when the compiler binaries or the `libc++` / `libc++abi` directory sit outside the usual `bin/` and `lib/` layout. `$ENV{HOME}` is expanded by CMake at configure time, so the path stays portable across machines; when you do spell all four out, [`variables`](#variables-variables) lets the shared prefix be written once. Of the targets these only affect `rest` and `json` — every other one builds with an off-the-shelf compiler and ignores them.

---

## Common gotchas

- **Paths are relative to the manifest file.** Move it and re-run `rosetta_gen`.
- A class missing from `classes[]` is **invisible** to the bindings.
- A bare-string target reuses `module_name`; if two object-form targets share a `name`, they share a module name — usually intended (one module per language), but watch for collisions across languages that write to the same directory.
- Comments must be valid JSON: use `"//"`-prefixed keys, not `//` line comments.
- The generator host always needs the C++26 / P2996 toolchain; only the `-expanded` and text targets build on an off-the-shelf compiler afterwards.