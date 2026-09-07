# Done

- **Annotation-pack visitor design.** The walker hands each visitor the full annotation pack — `v.template field<fld, Anns...>(name)`, `method_instance<fn, Anns...>`, `method_static<fn, Anns...>`, `constructor<ctor, Anns...>()` — and backends query it with `ann::has<A>(Anns...)` / `ann::get_or<A>(fallback, Anns...)`. Adding an annotation kind no longer touches the walker or every backend.
- **Enums.** `enum` / `enum class` are reflected with their enumerators (name + value).
- **Constructors.** Exposed via a `constructor<Ctor, Anns...>()` visitor entrypoint (public, non-copy/move, non-template, non-deleted). Copy/move/templated ctors are filtered.
- **Base classes / inherited members.** `walk<T>()` recurses public bases (`bases_of`) and flattens their fields + methods alongside `T`'s own, deduped by identifier: a derived declaration shadows the base one (most-derived wins) and a virtual diamond collapses to a single member (a `seen_types` guard keeps the walk linear). Annotations stay keyed on each member's *declaring* class, so a base member's inline + JSON side-car annotations are honoured. See `tests/inheritance.cpp`.
- **Virtual / override detection.** A synthesized `rosetta::virtual_spec{pure, overrides}` is injected into a method's annotation pack when its surviving reflection is virtual, so backends can tell a virtual / overriding method from a plain one (e.g. to emit a pybind11 trampoline). Other qualifiers (`const`, `noexcept`, ref-qualifiers) are still not surfaced — see below.

# Entities not visited at all

- Static data members. nonstatic_data_members_of skips them by definition, and there's no field_static visitor signature.
- Nested types (nested classes/structs/enums, type aliases). A Point::Coord enum nested in Point is invisible.
- Conversion operators and overloaded operators (operator(), operator+, operator[], …). operator() in particular is functor reflection — the project's prompt explicitly calls it out. The current filter rejects nothing for these by name, but there's no visitor signature distinguishing them, so backends can't route operator+ → __add__.

# Details ignored about what is visited

- Method qualifiers. `virtual` is now surfaced via `virtual_spec` (see Done). A `const` method, an `&&`-ref-qualified method, and a `noexcept` method still all reach method_instance<Fn> identically. Some backends care (pybind11 needs const-ness for py::const_; REST binding for safe vs. unsafe verbs). Same plumbing as `virtual_spec` — synthesize a marker into the annotation pack.
- ~~Parameter metadata~~ — **done**. `gen_detail::params_of` reads each parameter's `identifier_of` and `has_default_argument`; `GenParam` carries `name` (positional `argN` only as a fallback for a parameter declared without one), `has_default`, and a `default_text` the tool harvests textually, since P2996 reports the FACT of a default but not its expression. Consumed by `py::arg` / `nb::arg`, the `.d.ts`, C#/Java signatures, OpenAPI and the doc backends. See `tests/param_names.cpp`.
- Per-parameter annotations. `[[=range{0,1}]] double t` on a parameter is invisible — same plumbing as field annotations needs to repeat there.
- Bit-fields, mutable, anonymous unions. Niche but real; a generator that claims "full reflection" should at least flag them so backends can refuse cleanly rather than miscompile.
- Return-type metadata. The backend re-derives return_type_of(Fn) itself, fine — but `[[nodiscard]]`, ref/cv qualifiers, and `noexcept` get lost unless surfaced.
- Field traits. Is the type a std::optional, std::variant, container, smart pointer, or raw pointer? Each backend re-discovers this; centralising it in the walker (or in a tiny shape-classifier) would deduplicate a lot of backend code.

# A practical "what's next" punch list

1. ~~Annotation-pack refactor~~ — done.
2. ~~Enums~~ — done.
3. ~~Constructors~~ — done.
4. ~~Bases / inherited members~~ — done (with virtual/override detection).
5. Method qualifiers (`const` / `noexcept` / ref) + parameter names/defaults — the qualifiers follow the `virtual_spec` pattern exactly. (`const` / `noexcept` are now captured into the IR's `GenMethod` for trampoline signatures; still not surfaced to the *runtime* visitor pack.)
6. ~~Have a backend *consume* `virtual_spec`~~ — done: **Python** emits pybind11 trampolines (`PYBIND11_OVERRIDE[_PURE]`) and **Node** emits N-API trampolines (`Js_T : public T, NapiTrampoline` with a function-identity recursion guard). Both verified end-to-end — the generated module compiles and a Python/JS subclass override dispatches back through the C++ virtual (see `examples/trampoline` and `examples/trampoline-node`). Julia still ignores it; the N-API path carries a by-value-marshalling caveat (a trampolined type passed by value is sliced).
7. Operators & static fields / nested types.
8. ~~Parameter names / defaults~~ — done (item 5's second half). What is left of 5 is the qualifiers reaching the *runtime* visitor pack.
9. ~~Documentation for the generated binding~~ — done, but NOT via the walk: comments are not reflectable, so `rosetta_gen` reads them out of the header text (`tools/rosetta_gen/doccomments.cpp`) and `generate()` matches the result onto the reflected signatures. Per-parameter annotations (below) remain the reflection-side gap.

All remaining items are additive — no further walker-signature changes are required.
