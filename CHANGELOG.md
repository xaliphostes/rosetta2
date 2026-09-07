# Changelog

All notable changes to **rosetta** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This project has not yet cut a tagged release, so entries are grouped by date rather than by version number. Dates are `YYYY-MM-DD`.

## [Unreleased]

### Added
- **Parameter NAMES and DEFAULT ARGUMENTS reach the IR** — every `GenParam` used to be called `argN`: the walk decomposed a signature into types and threw the declaration away, so a generated SDK read `mesh.remesh(arg0, arg1)`, no backend could offer a keyword argument, and a C++ default simply did not exist on the other side of the boundary. For a tool whose output is meant to be a shipped SDK that was a visible quality ceiling, and it was the *only* thing standing between the IR and idiomatic output. `gen_detail::params_of` now reads each parameter's own identifier (`std::meta::identifier_of`) and whether it carries a default (`std::meta::has_default_argument`), with the old positional spelling kept as the fallback for a parameter declared without a name — keyed on its OWN index, so a half-named signature does not shift. It lands everywhere at once: `py::arg("radius")` / `nb::arg("radius")` in the Python family, real names in the `.d.ts` and in the C# / Java wrapper signatures, `title` in the OpenAPI parameter schemas, and readable labels in the ImGui / Qt inspectors and the `dynamic` metadata tables. Two hazards handled rather than hoped about: a C++ parameter may legally be called `function`, `string`, `params` or `final`, each a keyword in some target language, so TypeScript, C# and Java each run the name through their own guard and fall back to the positional form; and the obvious implementation — a helper templated on the parameter reflection — has its instantiations **collapse** under clang-p2996, every function in the TU reporting the names of whichever was instantiated last, so the walk is written as a `template for` and `tests/param_names.cpp` reads several differently-shaped signatures in one binary specifically to catch a regression to it.
- **Doc comments are harvested from your headers — `"doc_comments": true` (the default)** — a library bound with rosetta arrived undocumented. Everything the backends could say came from `rosetta::doc` annotations, which means the docstrings existed only if someone wrote them a second time, for rosetta, into a header that was supposed to stay untouched; meanwhile PMP, geogram and every other candidate library is already full of `///` and `@param`. P2996 reflects declarations, not comments, so this is a lexical pass in the TOOL (`tools/rosetta_gen/doccomments.cpp`) over the same headers `rosetta_gen` was already opening — `///`, `//!`, `/** */`, `/*! */`, trailing `///<`, `@brief`, `@param` (with `[in,out]` markers and multi-line text), `@return`, and `@note` / `@warning` / `@deprecated` / `@throws` / `@pre` / `@post` / `@see` as labelled paragraphs. The result reaches every backend: a Python docstring with `Args:` and `Returns:` sections, a TSDoc block with `@param` / `@returns`, Javadoc, C# XML docs, an OpenAPI operation `description`, a markdown/HTML per-argument list, and a class's own comment as the Python type's docstring (`GenClass::brief`). The same pass recovers what reflection cannot report at all: the **spelling** of a default argument, so `py::arg("segments") = 32` is a real Python default rather than a required argument.

  Nothing here is trusted on sight, because a scanner can be wrong and its output is spliced into generated C++. The harvest is matched against the reflected signature — by arity and by parameter names — and dropped when they disagree; two indistinguishable overloads document **neither**, since a plausible sentence on the wrong one is worse than none; a default's spelling is taken only where `has_default_argument` independently confirms there is a default to spell; and the spelling filter accepts literals and qualified-ids but refuses calls, operators and unqualified enumerators (`Fast`, which would not resolve where the binding writes it). The Python family adds one rule found by running the output rather than reading it: pybind converts an `arg` default into a Python object at **module import**, so an unregistered type there fails the import outright — defaults are emitted for numbers, booleans, strings, and enums bound in the same module, never for a class type or an unbound enum. Precedence is fill-never-overwrite: an inline `[[= rosetta::doc]]`, then a JSON side-car or a manifest `doc`, then the header — so adopting this cannot change a binding you have already annotated. Covered by `tests/doc_comments.cpp` (32 cases over the scanner, where the "declines to read" tests are as load-bearing as the "reads" ones) and `tests/doc_apply.cpp` (15 over the matching rules).

- **The manifest can declare VARIABLES — `"variables": [{"name": "P2996", "value": "..."}]`, used as `$P2996` / `${P2996}`** — a path that appears in several fields had to be written out in each of them, and the `cpp26_*` block made that concrete: one toolchain root spelled four times with a different suffix, so moving the fork meant editing four lines and hoping none was missed. A `variables` array declares a name for the shared part; the substitution runs over **every string in the document** before a single field is read, so it works anywhere a string does — `user_include`, `out_dir`, a `user_lib` directory, a `compile_definitions` value — and not only in the toolchain paths that motivated it. Declarations are an array rather than an object because they are ORDERED: a value may use the variables declared before it (`"BIN"` = `"$TC/build/bin"`), and only those — a forward reference is left as written. The field is erased once applied, so nothing downstream knows it existed. The load-bearing half is what is *not* substituted: a manifest already carries two dollar notations that belong to **CMake**, which expands them at its own configure time, so only a name the manifest DECLARED is touched — `$ENV{HOME}` is copied through whole (declaring `ENV` is an error, since `$ENV{...}` could never resolve to it) and an undeclared `${CMAKE_SOURCE_DIR}` is left exactly as written. That leniency has one cost, documented rather than papered over: a misspelt `$P2996x` is a name nothing declares, so it passes silently and surfaces later as a path that does not exist — `${P2996}x` is how a variable abuts text. Entirely optional and fully backward compatible: a manifest with no `variables` is byte-for-byte what it always was. Covered by `tests/manifest_vars.cpp` — 8 cases over the loader itself, which needs neither the fork nor reflection.
- **A class entry's `header` may be a GLOB — `{"header": "geom/*.h"}` binds the folder** — for the common library shape (one class per header, named after its file) the manifest repeated a line per class and had to be edited again for every header added to the tree; `rosetta_gen --init` filled that list once, from a scan, into a file that then drifted. An entry whose `header` carries glob magic now stands for every header it matches — one class entry per file, `name` from the file's stem — using the same pattern syntax `user_sources` already had (`*`, `?`, `[...]`, and `**` for zero or more directories), resolved against every `user_include` root with the composed `header_dir` in front, so it works inside a group and inherits that group's `namespace`. Optional `"exclude"` (a pattern or a list of them) drops the headers that declare no bound class. Only `.h/.hh/.hpp/.hxx` are taken, so `"**/*"` cannot drag a `.cpp` in, and a header whose stem is not a C++ identifier (`my-utils.h`) is skipped with a warning rather than emitted into a driver that will not compile. The per-class fields — `name`, `expose`, `annotations`, `extensions` — are rejected on a glob entry, since they cannot mean anything for a folder; `final` carries, being uniform. Globbed entries land after everything spelled out and yield to it, by name (the class is already bound, with whatever side-car or `expose` the entry gave it) and by header — an entry naming a class of a header **speaks for that header**, so a `types.h` declaring `Tolerance` and `Mode` can be listed class by class without the glob also inventing a `types`. Where that costs something it says so: a header declaring *both* a class named after the file and another one listed explicitly loses the first, and since the loader never opens a bound header only the author can say which was meant, so it prints a note naming the class that did not bind and the one-line fix. Entirely optional and fully backward compatible: a header path with no glob character takes exactly the path it always did. Covered by `tests/manifest_glob.cpp` — 11 cases over the loader itself (the first suite to compile `tools/rosetta_gen/manifest.cpp`, which needs neither the fork nor reflection).
- **`examples/dynamic` grows a solver — `scene::Relaxer`** — the demo library had a value type (`Vec3`) and a document (`Mesh`) but no *algorithm*, which is the case the dynamic projection is most useful for: a solver is mostly tuning parameters with declared ranges plus one method that runs for a while and mutates something else, i.e. exactly a property sheet nobody should have to write. `Relaxer` improves triangle shape by tangential Laplacian relaxation, projecting each displacement onto the vertex tangent plane so the surface slides rather than shrinks — on the Stanford bunny at 40 passes the bounding-box diagonal loses 1.05% with the projection off and 0.01% with it on. 25 passes at step 0.7 take the bunny's mean triangle quality from 0.834 to 0.912 and its worst triangle from 0.014 to 0.102. Adding it touched three files, all the library's own (`scene.h`, `Mesh.ann.json` for one new accessor, and one line of `manifest.json`); no consumer changed — not `qt/`, not `interp.h`, not any of the five scriptable drivers — yet all of them gained it. `paper/experimental-eval/E6-case-study` checks that mechanically: the same library through the pipeline twice, differing only in whether the manifest lists the solver, asserting the metadata *does* move (4 files) and the stage-2 binding of `rosetta::script` does *not* (0 of 18 files, 4173 lines).
- **`coverage.json` records FREE FUNCTIONS** — every backend tracked class members and nothing else, so a manifest's `functions` were invisible to the coverage report entirely: not counted as bound, and silently dropped when a backend's gates rejected them. On a library whose algorithms are free functions over one data structure that is most of the API — for [PMP](https://github.com/pmp-library/pmp-library), 99 of its 120 nameable entities — so the artifact [docs/COVERAGE.md](docs/COVERAGE.md) describes as a complete account of what did and did not bind was covering a fifth of it. Adds `coverage::note_bound_function` / `note_skip_function` and a per-target `"functions": {"bound": [...], "skipped": [...]}` array beside `"classes"`; the `python`, `nanobind`, `julia`, `lua`, `node`, `wasm`, `csharp`, `java`, `typescript` and `dynamic` backends all call them. Purely additive — the existing `classes` array, the `bound`/`skipped` scalars and the reflection section are unchanged, so existing readers keep working. Cross-validated against emitted output: the pybind11 backend reports 64 bound free functions and `auto_pybind.cpp` contains exactly 64 `m.def(` calls.

### Fixed
- **A docstring containing a newline no longer breaks the emitted C++** — `py_lit`, which the python, nanobind, imgui and qt emitters all escape their docstrings with, handled backslashes and quotes only. That held for as long as every docstring came from a `doc{"..."}` annotation, which is single-line by construction — its source is itself a C++ string literal. It stopped holding the moment a docstring could come from a `/** ... */` block spanning paragraphs: a raw newline inside a `"..."` literal does not compile. Control characters are now escaped too, and the C# and Java emitters, which wrote a doc into a single-line `/// <summary>` and a single-line `/** */`, render proper multi-line blocks (with XML escaping on the C# side, and `*/` neutralized on the Java side).
- **`examples/dynamic`: `Mesh::sphere()` and `Mesh::bunny()` wound their triangles inside-out** — the two triangles of each quad were emitted as `{a, b, a+1}` / `{a+1, b, b+1}`, which is clockwise seen from outside, so every face normal `cross(b-a, c-a)` pointed *into* the sphere. The visible consequences were subtle enough to live there a while: the diffuse term lit the side facing *away* from the light, and — because the half-vector `H` then never aligned with any visible normal — the specular term was exactly zero on every fragment a camera could see. Wound counter-clockwise now. `bunny()` had the same defect for a different reason — the scan's index table in `bunny.h` is itself clockwise — and is now reversed as it is read. The cube and plane were already correct. Checked rather than eyeballed: for a closed mesh the signed volume `sum(dot(a, cross(b,c)))/6` is positive exactly when the winding is outward, and it was `-0.195` for the bunny and `+1.0` / `+0.513` for the cube and sphere; all three are positive now.
- **`examples/dynamic`: the Qt viewer ignored the `preset` field** — `sceneview.h` read `visible`, `colour`, `opacity`, `size`, `spin`, `origin` and `shading`, so the Preset radio row (`default` / `matte` / `glossy` / `glass`) set the field and changed nothing on screen. It now maps the preset to a material — specular exponent, specular strength, a Fresnel rim and an alpha multiplier — passed as three new shader uniforms: `matte` has no highlight, `glossy` a broad sheen, `glass` goes translucent with a bright rim. The names are *not* hard-coded against the library: they come from the field's `combobox` annotation, an unrecognised name falls back to the default material, and a class with no `preset` field renders exactly as before, because the view reads it with the same optional-field fallback it uses for colour and opacity. Note this was masked by the winding bug above — with inverted normals no specular setting could have shown regardless.
- **The `typescript` backend declared free functions through no gate at all** — its per-method loop applied the runtime backends' visibility rule, but its per-function loop emitted every entry unconditionally, so the `.d.ts` declared functions the N-API module it describes does not bind. That is the one thing the backend's own source comment says it must not do ("declaring the others would promise the caller a method that is not there"). Free functions now take the same rule methods do. Known limitation, unchanged: `typescript` re-implements node's marshalling gates approximately rather than sharing them, so it still declares 90 free functions where `node` binds 82 — narrower than the 91 it declared before, but not closed.
- **The `typescript` backend records what it BINDS, not only what it skips** — it called `coverage::emit_overload` (so overload drops appeared) but never `note_bound`, so its `bound` count in `coverage.json` was structurally zero however much it emitted, and the two visibility gates below that — a non-adaptable sequence, a non-copyable class by value — dropped members without recording anything at all. Its coverage figure was therefore unreadable in both directions: no target could be compared against it, and its silent drops were invisible. It now calls `note_bound` / `note_bound_field` on every declaration it writes and `note_skip` / `note_skip_field` with `sequence_not_adaptable` or `unmarshalable_type` on every one it hides. Re-running E1 (`paper/experimental-eval/E1-overload-recovery`) turns a row of zeros into `typescript` tracking `node` exactly — 96 bound, 192 overload drops at five overloads per name — which is what the backend was emitting all along. Note the asymmetry this leaves: `typescript` and `dynamic` record bound *fields*, the other language backends record only methods, so the top-level `bound` scalar is not comparable across those two groups; count methods by signature, as `E1/analyse.py` does.
- **Generated projects default to `Release` instead of to no build type at all** — every compiled backend's `CMakeLists.txt` emitted a `CMAKE_BUILD_TYPE` block only when the manifest set `build_type`, so the documented two-line build (`cmake -S bindings/python -B build && cmake --build build`) configured CMake with *no* build type, hence no `-O` and no `-DNDEBUG`, and shipped an unoptimized module with nothing in the output hinting at it. Measured on a pybind11 module, a trivial call cost ~707 ns that way against ~84 ns at `Release`. A binding is a redistributable artifact — someone building a wheel the documented way was publishing a slow one — so the no-op default was a footgun rather than a neutral choice. The block is now always emitted, defaulting to `Release`, and is additionally guarded on `CMAKE_CONFIGURATION_TYPES` so multi-config generators (Visual Studio, Xcode) still pick their configuration at build time. `-DCMAKE_BUILD_TYPE=...` at configure time and the manifest's own `build_type` both still win; pass `Debug` explicitly to get the old flags.

### Changed
- **`wheel` and `wheel_dir` moved from the top level onto the TARGET** — **breaking.** They began life as manifest-side defaults for `--build`'s `--wheel` / `--wheel-dir`, and a flag applies to a whole run, so the manifest counterpart was global to match. That reasoning did not survive the rest of the format: `out_dir` has exactly the same shape — where an artifact lands — and is a top-level default with a per-target override, and half the wheel configuration was per-target already, since `python` on a target pins the interpreter the wheel is *tagged for* (`build.cpp` runs `make_wheel.py` through `target_python(t, …)` precisely so a 3.11 build does not emit a `cp312` wheel). So a manifest could say which interpreter each Python target packages with, but not whether it packages at all. The case that exposes it: `nanobind` emits one `abi3` wheel covering every CPython 3.12+ where `python` emits one per version, so shipping the first and not the second — or sending them to different directories — is a real thing to want, and it needed two `--build --only … --wheel` runs. Both keys now sit on the `python` / `nanobind` entry of `targets`; any other `lang` carrying one is a load error, since no other backend emits a `make_wheel.py` to run. The flags keep their one-directional rule: `--wheel` packages every wheel-capable target whatever the manifest says (a target spelling `"wheel": false` cannot disarm it) and `--wheel-dir` overrides every target's own directory, which is what keeps "collect them all in one place" a single flag. **Migration is one edit, and the tool names it:** a top-level `wheel` / `wheel_dir` is rejected with `move it onto the python / nanobind entries of "targets"` rather than ignored — a manifest that used to ship wheels must not quietly stop. Covered by `tests/manifest_wheel.cpp` (8 cases, including the migration guard); `--init` now shows the fields on its python target.
- **`std::vector<T>` crosses the wasm boundary as a plain JS `Array`** — the embind backend emitted `register_vector<T>`, which made it an *opaque bound class*: JS had to fill an `M.vector_double` with `push_back`, could not pass an array literal, and got a handle (not an `Array`) back, one it then had to `.delete()`. It was the one backend where a sequence was not the host language's own array type, contradicting what [docs/MANIFEST.md](docs/MANIFEST.md) already promised. The generated source now specializes embind's `BindingType<std::vector<T>>` onto the emval wire — `val::array` out (a typed-array memcpy for arithmetic `T`), `vecFromJSArray` in — and registers each element type with `register_type` instead. embind's own `const T` / `T&` forwarders carry it to `const std::vector<T>&` parameters and returns, so plain member-function pointers still bind and nested `vector<vector<T>>` falls out by recursion. **Breaking for hand-written JS**: `M.vector_double` and friends no longer exist; pass and expect arrays. `examples/plain-binding/drive.wasm.js` is now `drive.js` line for line, printing identical output.
- **The IR accessors are public** — `qualified_of` / `exposed_of` / `exposed_object_of` / `names_type` moved from `rosetta::gen_detail` to `rosetta::`, where the rest of the backend API already was, with the old spelling kept as `using`-declarations onto the same functions.
- **The header-only runtimes moved to `include/rosetta/runtime/`** — the five reflection-free headers the *generated* project compiles (`node`, `csharp`, `java`, `qt_widgets`, `imgui`) left `backends/` and `visitors/`, neither of which described them.
- **Backends dropped the `_backend` suffix and moved to `rosetta::backend`** — `backends/python_backend.h` → `backends/python.h` and `gen_detail::PythonBackend` → `backend::Python` across all 19, with `visitors/*_visitor.{h,hxx}` → `visitors/*.{h,hxx}` to match.
- **The last four `-expanded` names are gone** — `lua`, `qt`, `qml` and `imgui` dropped a suffix that had never distinguished them from a thin twin they never had, with the old spellings still resolving as deprecated aliases.

## 2026-08-15

### Removed
- **The seven *thin* backends are gone** — `python`, `nanobind`, `node`, `wasm`, `julia`, `csharp` and `java` now mean what `*-expanded` meant, deleting 14 backend files and the 14 reflection visitors only they used (~4800 lines).
- **Migration is a no-op for existing manifests** — the `*-expanded` spellings stay registered as aliases and `rosetta_gen` folds them to the short name, though output directories follow the short name, so a tree generated earlier has stale `*-expanded/` directories to remove.

### Fixed
- **nanobind bindings register their base class** — `nb::class_<T>` never declared the bases the IR had carried all along, so every derived class was an unrelated Python type and passing one where a base was expected failed.
- `collect_members` now enumerates through `access_context::unchecked()`
- **A class with a non-public destructor is skipped, not compiled** — every runtime backend destroys what it wraps, so listing such a class failed the build with an error pointing inside `<__memory/unique_ptr.h>`.
- **A `compile_definitions` value may contain quotes** — `GEOGRAM_VERSION="1.10.1"` was emitted inside a plain string literal that ended at the value's own first quote; it is a raw string literal now.
- **A class-typed return whose class is not bound no longer binds (python / node)** — pybind11 threw on the first call and node took the whole process down with a fatal N-API error, where both now skip the member and record it in `coverage.json`.
- **An overload whose exact signature has no spelling is skipped rather than emitted** — a disambiguating `static_cast` naming a lambda closure type produced a *compile* error, where every other gate in rosetta produces a skip.

### Added
- **Out-parameters — `"out_params": {"AttributesManager::get_doubles": [1, 2]}`** — a parameter the method returns through is filled by an emitted local and joins the return value, as a tuple in Python, multiple returns in Lua, an array in JS and a declared tuple in TypeScript.
- **`module_init` — the manifest can say what runs when the module LOADS** — statements spliced verbatim at the top of the module entry point, for the library start-up work that is not expressible as "bind this name".
- **`generated_headers` — a header the bound library's own build system would have produced** — a manifest entry names a template plus substitutions (or literal content), and the finished text is written into the binding tree, first on the include path.
- **`std::filesystem::path` marshals as a string, and `std::shared_ptr<T>` reaches the caster-less backends** — the two independent gaps that had kept a factory API like `std::shared_ptr<Mesh> compile_file(const std::filesystem::path&)` from binding on any target.
- **Overload selection on FREE functions — `"signature": "void(GEO::Mesh&, bool)"`** — a manifest entry may name the one overload it means, since `^^GEO::mesh_union` is ill-formed the moment the namespace declares the name twice.
- **Overload support — a class's whole overload set now reaches the binding** — the walk deduplicated member functions by identifier alone, silently dropping every overload but one.
- **`coverage.json` — a machine-readable account of what bound and what did not** — written on every run, recording reflection-stage drops, per-target skips with a reason, and what did bind, so a method that stops binding is a reviewable diff ([docs/COVERAGE.md](docs/COVERAGE.md)).

## 2026-08-09

### Fixed
- **A bound class template specialization was spelled with its template arguments stripped of every namespace** — `display_string_of` is a *display* rendering rather than a C++ spelling, so the template-id is now composed structurally from `template_of` + `template_arguments_of`.
- **csharp / java emitted a member pointer that could not compile for any overloaded method** — the bare `&T::f` names the whole overload set, so both now spell it through the shared cast to the exact signature.
- **nanobind: the abi3 wheel was *tagged* but not *built*** — nanobind discards a `STABLE_ABI` request without a word when `Development.SABIModule` is not among the requested components, so the wheel installed on every CPython ≥ 3.12 and imported on exactly one.
- **A class nested inside another class is spelled fully qualified (`GenClass::qualified`)** — `class_namespace<T>()` stops at the first non-namespace scope, so the `name_space::name` reconstruction collapsed to a bare identifier the emitted code cannot resolve.
- **`qualified_class_name<T>()` handles a class template specialization** — a specialization has no plain identifier, so the unguarded `identifier_of` stopped the enclosing `consteval` lambda from being a constant expression.

### Added
- In **rosetta_gen**, added option `--jobs N, -jN` for parallel build jobs
- **Pinning the runtime — per-target `"python"` / `"requires_python"` / `"napi_version"` / `"node_engine"`** — a generated project used to take whatever `PATH` handed it, and the obvious `-DPython_EXECUTABLE` escape hatch was silently overwritten by the template's own probe.
- **Where the built artifact lands — per-target `"out_dir"`** — a target may name the directory its artifact is copied to after every build, instead of every project bolting on a copy step that has to know each backend's build layout.
- **`"wheel"` / `"wheel_dir"` in the manifest** — packaging was command-line-only, so both are now manifest fields acting as defaults that the flags can still override, but only ever toward packaging more.
- **The caster-less backends reach interop types too — `"sequences": [{ "type": "Eigen::VectorXd" }]`** — a concrete type may state its own C++ spelling and cross node / wasm / lua through the flat-array adapter, while the Python family keeps its native caster.
- **Foreign 2-D matrices — `"matrices": [{ "type": "Eigen::MatrixXd" }]`** — `is_sequence` one dimension up, marshalled as an array of rows through a `std::vector<std::vector<element>>` boundary on every expanded backend.
- **Foreign-library interop — `"interop": ["eigen"]` binds Eigen types natively (python / nanobind)** — recognition is by enclosing namespace rather than a list of type names, so one line covers every Eigen spelling including the ones a hand-maintained list would have missed.
- **Python wheels — `python` / `nanobind` emit `pyproject.toml` + `make_wheel.py`** — the backends produced a buildable CMake project but nothing installable, and `rosetta_gen --build` gained `--wheel` / `--wheel-dir` to drive the packaging.
- **`std::shared_ptr<T>` crosses the boundary — holders declared automatically (python)** — pybind11 refuses to hand out a `shared_ptr` whose pointee was not registered with a matching holder, so the holder set is computed and propagated along bound base links.

## 2026-07-31

### Fixed
- **A non-const `T&` return no longer binds as a COPY (python / nanobind)** — a fluent API handed Python a copy on every call, measured at 3.66 s for a 50k-point loop against 5.5 ms once `reference_internal` was attached.
- **Enumerations nested inside a class are spelled fully qualified (`GenEnum::qualified`)** — the same shape as the nested-class fix, one level down and in the IR rather than in a backend.
- **nanobind: the STL type casters are actually included** — nanobind splits its casters one per header where pybind11 ships them together, so every standard type outside `string` / `vector` / `function` compiled and then threw at call time.
- **pybind11 trampolines: a return type containing a comma no longer mis-expands** — `PYBIND11_OVERRIDE` is a macro, so `vector<T, allocator<T>>` was split across two macro arguments; such a return type now hides behind a local alias.
- **nanobind: no constructor is emitted for an abstract class** — `nb::init<>` instantiates the alias type, so `new T{}` on a pure-virtual interface is ill-formed.

`tests/shared_ptr.cpp` covers all four with 14 gtests over the generated sources, 9 of which fail against the pre-fix backends.

### Added
- **`"expose"` now reaches every backend — and works on enums** — the class rename was consumed by only 7 of 26 backends, and a rename on an enum entry was parsed, emitted into `bindings.h`, and then silently ignored by all of them.
- **`"expose"` on free functions and extension methods** — two free functions sharing an unqualified name had no fix and degraded differently per backend, pybind making an overload set where node simply kept the last.
- **Per-class `"expose"` — bind a class under a different name** — and emitted C++ now spells every bound class qualified, since the unqualified spelling became ambiguous the moment two bound namespaces declared the same identifier.
- **Manifest `user_lib` accepts an array** — a bound library plus the pre-built libraries it depends on, linked in array order with one de-duplicated rpath list.
- **`user_sources` globstar — `**` matches zero or more directories** — so one entry replaces a `*.cxx` line per subdirectory.
- **Manifest grouped entries — scoped defaults inside `classes` / `functions`** — an element may be a group carrying `entries` plus local `namespace` / `header_dir` / `header` defaults, nestable and resolved entirely in `load()`.
- **Manifest `namespace` / `header_dir` — shared defaults for class & function entries** — two optional top-level fields factor the per-entry repetition out of `classes`, `functions` and `extensions`.
- **`rosetta_gen --init <src_dir>` — pre-fill the manifest from a source scan** — a comment- and preprocessor-aware token walk rather than a real C++ parse, skipping what it cannot bind with a note where useful.

### Changed
- **`rosetta_gen` split into one file per concern** — ~2400 lines of `rosetta_gen.cpp` became `manifest` / `emit` / `init` / `build` / `clean` / `util`, verified to produce byte-identical output.
- **Plain `rosetta_gen` emits incrementally and guides the next step** — unchanged files are no longer rewritten, and both footguns of the manual three-step workflow now print a note.

## 2026-07-20

### Added
- **Manifest `build_type` / `optimization` — build configuration for every generated CMakeLists** — emitted as a default inside `if(NOT CMAKE_BUILD_TYPE)` and as `add_compile_options` / `add_link_options` respectively, so a configure-time `-D` still wins.

### Fixed
- **wasm templates: manifest `link_options` land after the template's own link flags** — emcc honors the *last* occurrence of a repeated `-s` setting, so an override could never take effect before.
- **Expanded backends: nested class types are spelled fully qualified** — the emitted code opens namespaces with `using namespace` but cannot open classes, so a bare nested identifier in a constructor or trampoline signature failed to compile.
- **Constructor parameters of std-namespace types are spelled by value — dangling-view fix** — bindings pass caster temporaries, so a constructor that *stored* a `const std::vector<double>&` captured freed memory.
- **The walk skips `std::initializer_list` constructors** — no target language can produce one, and such a constructor always shadows an equivalent `std::vector` overload.
- **Node: a non-default-constructible class is constructible from script** — `Wrap` default-constructed and then assigned, so a class whose only constructors take arguments threw "this class cannot be constructed directly".

### Changed
- **Expanded backends emit `CMAKE_CXX_STANDARD 20`** — the templates still pinned C++17, so a bound library using `std::span` or concepts in its headers failed to compile.

All four fixes verified end to end on the stressinv manifest (20 classes, 14 free functions); suite 56/56.

## 2026-07-16

### Added
- **`rosetta_gen --build` — the whole manifest pipeline in one command** — emits, builds and runs the generator, then compiles every declared backend with its own build shape, skipping the ones whose toolchain is absent; the companion `--clean` removes only what the pipeline generated.
- **`rosetta_gen` builds on Windows** — the `user_sources` glob expansion dropped POSIX `<glob.h>` for a `std::filesystem` walk with the same semantics.
- **Per-class `"final": true` manifest flag — suppress the trampoline** — a class marked final generates no trampoline, which also makes a virtual-bearing class eligible as a node member-object property.
- **Foreign sequence containers (`rosetta::is_sequence` trait + manifest `sequences` field)** — a library's own vector type crosses the boundary by copy through a `std::vector<element>` adapter, like a `std::vector` of its element.
- **Member-object properties on wasm** — the non-copyable member object of a bound class binds as a borrowed-handle getter method, since embind properties copy.
- **Member-object properties — a class's non-copyable member objects become reference properties** — bound read-only against the real member, with the parent kept alive for as long as any child handle exists.

### Fixed
- **Overload sets no longer break the expanded builds** — the bare `&T::name` member pointer is ambiguous for an overload set, so binding any class with a const/non-const accessor pair failed to compile.

### Changed
- `node_runtime.h` now holds only the documented declarations, with the conversion helpers, the override plumbing and all of `Wrap`'s member definitions moved to its inline half (the `generate.h` / `inline/generate.hxx` layout convention).

## 2026-07-08

### Fixed
- **Out-of-line side-car: scientific notation in numbers** — the consteval JSON parser had no exponent support, so a `"range": [1e-10, 1e-6]` parsed as `[1, 0]` and silently derailed every key after it.
- **Emitted range bounds lose tiny magnitudes** — `std::to_string` collapses `1e-10` to `0.000000`, so bounds are formatted with `%g` now.

### Added
- **Four new widget hints: `color`, `multiline`, `radio`, `file`** — implemented across all three UI inspector backends, inline and out of line alike.
- **Out-of-line side-cars reach annotation parity** — `.ann.json` now also carries `label`, `button` and `widget`, so a plain-C++ header plus a side-car drives exactly the same UI as inline annotations.
- **Dear ImGui backend (`imgui`)** — the immediate-mode counterpart of the Qt inspector: a self-contained GLFW + OpenGL 3 app whose generated `draw_<Class>()` functions bind widgets directly to the live members, no copies and no signal plumbing.
- **Julia backend, expanded variant (CxxWrap / jlcxx)** — building against the *off-the-shelf* libc++ lets `<jlcxx/stl.hpp>` compile, so `std::vector` is fully bound where the thin target had to skip it entirely.
- **Lua backend (sol2)** — a `require`-able C module for Lua 5.1–5.4 / LuaJIT built with an off-the-shelf C++17 compiler, with full runtime constructor overload resolution and multiple inheritance via `sol::bases`.
- **Extension methods (`extensions` manifest class field)** — a free function whose first parameter is `Cls&` is exposed as an ordinary instance method, the escape hatch for a library whose own members cannot cross the boundary.
- **Copyability captured in the IR, and emitters gate on it** — every runtime backend copies at some boundary, so a class whose copy is deleted used to make the *generated code* fail to compile.
- **Per-target `link_options` manifest field** — extra linker flags for that target only, since link flags are toolchain-specific in a way `compile_definitions` is not.
- **`compile_definitions` manifest field** — preprocessor definitions applied to the driver and every compiled binding target, so the walk, the bound headers and the `user_sources` all see the same preprocessor state.
- **C sources in `user_sources`** — `.c` entries make the generated CMakeLists call `enable_language(C)` so they compile alongside the C++ binding.
- **Node / WebAssembly: JS-function callbacks are marshalled into `std::function` parameters** — bound only when the whole signature is convertible, so an unconvertible one is skipped rather than breaking the build.
- **Node / WebAssembly: raw pointers to bound classes are marshalled** — as a non-owning handle, with the C++ side keeping ownership.
- **Python / WebAssembly: C++ inheritance is registered** — a derived instance is now accepted wherever a base pointer or reference is expected, and the hierarchy is visible to the host language.
- **`rosetta_gen --init` flag** — writes a fully-commented example `manifest.json`, refusing to overwrite an existing one.
- **`user_sources` manifest field** — user `.cpp` files compiled directly into every generated binding target, with shell globs expanded at generation time.
- **Multiple include directories** — `user_include` accepts an array of directories, searched like a compiler's `-I` order.

### Fixed
- **Generator link no longer requires the bound class's constructor** — the IR visitor default-constructed a `T tmp{}` to read every field's default, odr-using a constructor the driver links no library for.
- **Node: the `Wrap` constructor compiles for non-assignable classes** — its parameterized path is now `if constexpr`-guarded on assignability, matching the emitter's own gate.
- **Node: wrapped class arguments are handed to C++ by reference, not by copy** — so a function taking `T&` mutates the caller-visible JS object, and a pImpl facade no longer dangles its duplicated impl pointer.
- **WebAssembly: several build breakers for a real library** — C++20, by-value marshalling restricted to bound classes, no constructor for an abstract class, `static_cast` disambiguation of member pointers, and at most one constructor per arity.
- **Node: by-value class marshalling requires a round-trippable type** — complete, default-constructible and copy-assignable, or the member is skipped rather than breaking the build.
- **Python / Node: trampoline override signatures resolve unqualified `std` names** — the trampoline block emits a `using namespace std;` scoped to its own namespace, with no leak into the registration code or user headers.
- **Python / Node: trampolines skip virtuals they can't marshal** — a new `GenMethod::sig_bindable` flag omits the overrides whose signature has no caster, keeping the trampoline instantiable.
- **Python / nanobind / WebAssembly / Qt / QML: unbindable members no longer break the module** — raw C arrays and types only forward-declared in the binding TU are skipped instead of aborting the whole build.
- **Python / nanobind / WebAssembly / Qt / QML: a pointer is judged by its pointee, and `std::vector` by its element** — `sizeof(T*)` is always valid, so a pointer to an incomplete type slipped through the old guard and broke the build inside the framework's caster.
- **Python / nanobind / WebAssembly / Julia: the synthesized default constructor was registered behind a runtime `if`** — a plain `if` still instantiates the discarded branch, a hard error for a type whose only constructors take arguments.
- **Node: reference parameters of bound types** — a new `arg_from_napi<P>` binds them to the wrapper's persistent object, so in-place mutations propagate back to the JS object.
- **Node: unbindable free functions are skipped, not fatal** — the module still loads and every other function stays usable.
- **Template-specialization type names** — `class_name<T>()` falls back to the full display spelling when the type has no plain identifier, instead of hard-erroring.
- **Operator / conversion members** — the member walk skips functions without an identifier, which cannot be bound by name to a target language.

### Changed
- `annotate.h` now holds only the customization points, the parsed-representation types and documented declarations, with the consteval JSON parser and the walk-time merge implementations moved to `inline/annotate.hxx`.

### Docs
- Added `docs/ANNOTATIONS.md` — the companion reference to `MANIFEST.md` for the annotation layer: the full set, inline vs out-of-line spelling side by side, which backend consumes what, and the gotchas.
- Documented `user_sources`, multiple `user_include` directories, and added an initial `docs/MANIFEST.md` reference for the manifest file.
- Documented per-target `link_options`, class `extensions` and the skip-instead-of-fail semantics for unmarshalable members in `docs/MANIFEST.md`.
- Added this `CHANGELOG.md`.

## 2026-06-27

### Added
- External third-party library linking: full example using both dynamic and static linkage, with handling for libraries that live in their own namespace.

### Changed
- Reworked dynamic-vs-static user-library linkage; namespaced third-party libraries now bind without qualifying every spelling.

## 2026-06-26

### Added
- **Java backend and visitor** — C-ABI shared library plus handle-backed FFM wrappers.
- More backend examples wired up for the `geom-lib` example.

### Docs
- README updates, including the backend capability table.

## 2026-06-25

### Added
- **C# backend** — C-ABI shared library plus P/Invoke wrappers, buildable without a C++26 toolchain (expanded path).

## 2026-06-20 – 2026-06-21

### Added
- **nanobind** visitor/backend support, plus a `nanobind-expanded` variant.
- **Expanded backends** — reflection runs once on a C++26 host and the generated sources build with an off-the-shelf compiler, for toolchains that don't yet support C++26 / P2996 reflection.
- Browser example for the WebAssembly target.

### Changed
- "Transparent rosetta" pass over the generation flow.

## 2026-06-16 – 2026-06-19

### Added
- **Inheritance introspection** — base-class flattening, `virtual_spec` carried through the walk, and trampolines for pybind11 and Node so virtual/overriding methods are distinguished from plain ones.
- **ParaView** Server Manager plugin XML generation.

### Changed
- Refactored doc generation.

### Docs
- README updates.

## 2026-06-12 – 2026-06-14

### Added
- **Out-of-line (external file) annotations** — annotate bound types from a JSON side-car so the headers stay clean.
- GoogleTest integration and `signal::scoped_connect`.

### Changed
- Reorganized sources; simplified the mini-MOC signal handling and refactored the mini-MOC.
- Moved the reflection walk into an inline `walk.hxx`.

### Removed
- Obsolete files.

## 2026-06-09 – 2026-06-11

### Added
- **Julia** language binding/backend (CxxWrap.jl / jlcxx).

### Changed
- Moved the Qt/QML inspectors under `include/rosetta`; general reorganization and a linter pass.

### Docs
- README and Julia example updates.

## 2026-06-02 – 2026-06-08

### Added
- **Enum** support.
- **Free (non-member) function** binding.
- **REST** backend (cpp-httplib JSON server + browser client) and a JSON (de)serializer.
- **OpenAPI 3.1** spec backend.
- **TypeScript** (`.d.ts`) and **Markdown** documentation backends.

### Changed
- Split backends into separate files; renamed the `web` target to `wasm`.
- Inlined the doc generator.

## 2026-05-31 – 2026-06-01

### Added
- The `rosetta_gen` manifest-driven project generator.
- Constructor binding support.

## 2026-05-29 – 2026-05-30

### Added
- CLI tools for generating skeletons.
- Richer annotations and an initial Qt/QML example.

### Docs
- Early documentation.

## 2026-05-28

### Added
- Initial commit: the rosetta framework and introductory slides.

[Unreleased]: https://github.com/Xaliphostes/rosetta/compare/df8960d...HEAD
