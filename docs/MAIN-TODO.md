# 1. The C++ surface it can express

Overloads collapse silently. collect_members dedups methods by identifier (include/rosetta/inline/walk.hxx:98-105) — first declaration wins, the rest vanish with no diagnostic. at(i) / at(i,j), set(x) / set(x,y) — one survives. Constructors are the exception (all bound). This is the single most likely thing to surprise someone binding an existing library, and MAIN-TODOs #5 already names it for mesh_load.

Operators and conversions are structurally out. is_exportable_member_function gates on has_identifier (include/rosetta/walk.h:74), so operator==, operator[], operator+, operator T never reach a backend. Combined with the absence of __iter__/__len__/__repr__ synthesis, generated Python/Lua/JS APIs are mechanically correct but never idiomatic — p.equals(q) instead of p == q, no indexing, no printing.

The container vocabulary is thin. type_descriptor (include/rosetta/inline/generate.hxx:903-1049) understands: void, bool, string, arithmetic, std::function, std::vector, registered sequence/matrix traits, enum, raw T*, shared_ptr, class. Everything else falls through to kind = "unknown" → member skipped. That means no std::map, unordered_map, set, optional, variant, tuple, pair, array, span, string_view, unique_ptr, chrono. map and optional alone will silently delete a large fraction of methods on a typical modern C++ API.

~~No parameter names, no default arguments.~~ **Fixed.** `params_of` reads `identifier_of(param)` and `has_default_argument(param)`, so `GenParam::name` is the declared identifier (`argN` only where the declaration gives none) and `has_default` marks the optional ones. The default's *spelling* is not reflectable at all, so it is harvested from the header text and cross-checked against `has_default` before it is used. Python gets `py::arg("radius") = 32`, TypeScript gets `radius?: number`, C#/Java get real signatures, OpenAPI gets named parameters. See `tests/param_names.cpp`.

Documentation, likewise, is no longer only what someone re-typed as a `rosetta::doc` annotation: `"doc_comments"` (on by default) reads the `///` and `/** @param … */` blocks the bound library already has and feeds every backend. See `docs/MANIFEST.md#doc-comments-doc_comments`.

# 2. Semantics across the boundary

No threading story at all. Zero hits for gil_scoped_release, call_guard, or ThreadSafeFunction across the whole tree. Two consequences: any long C++ call holds the GIL and freezes the interpreter, and a std::function callback invoked from a C++ worker thread into Python/JS is undefined behavior. For rosetta's apparent target audience — geometry/numerics libraries (geogram, Eigen interop) that are parallel by nature — this is the sleeper blocker.

Callbacks are a 3-of-27 feature. is_callback is honored only in lua_expanded, python_expanded, wasm_expanded. Node, Julia, C#, Java skip any method taking a std::function — so "register a progress handler" doesn't exist there.

Reference/ownership semantics are half-built. MAIN-TODOs #2 is accurate: node's Wrap<T> stores by value so borrowed sub-object handles can't be represented, wasm returns raw pointers with no parent pin (dangling handle if the owner dies first), and there's no dangling policy stated anywhere. unique_ptr returns aren't handled at all.

# 3. The structural limit: 27 × N

Every capability above has to be implemented once per backend, and the matrix is already ragged — is_pointer in 4 backends, is_callback in 3, ⚠️ on Julia/Wasm C++26 in the README table. There's no shared marshalling layer that a backend opts into; each backends/inline/*.hxx re-derives what it can express (~16.5k lines across them). The cost of "support std::optional" is O(backends), not O(1). This is the thing that decides whether rosetta scales past its current feature set, and it's worth deciding deliberately: a tiered model (a small core of blessed backends that get every feature, the rest explicitly best-effort) is more honest and cheaper than an implied 27-way parity promise.

Silent skipping needs an audit artifact. The skip-don't-emit-a-throwing-binding policy is the right call — but today the user discovers which of their 40 methods actually bound by reading generated code. Your own CHANGELOG names this bug class twice (the abi3 wheel, the expose/extension key mismatch): a request accepted and then silently discarded. A per-generation coverage report (bound / skipped + reason) would convert the entire class into something a diff catches.

# 4. Toolchain and validation

Generation requires parsing your whole header closure in C++26 mode under an experimental fork. The expanded backends are a genuinely strong mitigation — generate once, build anywhere with off-the-shelf toolchains — but the generation host still has to get clang-p2996 through every transitive include. Heavy TMP, boost, CUDA, MSVC-isms, or PCH-dependent headers block generation, and diagnostics inside a consteval walk are famously poor. P2996 is also still moving; annotation and splice spelling can shift before C++26 ships, which is real churn on walk.hxx / generate.hxx.

Validation is the weakest link relative to the claim. 18 test files, macOS-only, and the only GitHub workflow is marp.yml (slides) — nothing runs the test suite, and nothing verifies that a generated Python/Node/Wasm module imports and runs on Linux or Windows. The headline promise is "ship the generated sources anywhere with an off-the-shelf toolchain"; that's precisely the axis with no automated evidence behind it.

If I had to pick three

1. Overload support + a machine-readable coverage report — kills the largest silent-failure class, and the report is cheap.
2. std::optional / std::map in type_descriptor — the most common reason a real API's methods disappear today.
3. A generated-module smoke test in CI on Linux — one backend, one example, import and call. It defends the core claim.

Threading/GIL is fourth only because it's expensive; it's the one that will hurt most once someone binds a library that actually computes.
