// Copyright (c) fmaerten@gmail.com
// License: MIT

// Reflection-driven binding scaffolder. `rosetta::generate<T>(...)` reads
// the `rosetta::binding_info<T>` trait specialization and emits a
// per-backend project tree under <out_dir>:
//
//   <out_dir>/python/{auto_pybind.cpp, CMakeLists.txt, README.md}
//   <out_dir>/node/  {auto_napi.cpp,   CMakeLists.txt, package.json, README.md}
//   <out_dir>/rest/  {auto_rest.cpp,   CMakeLists.txt, README.md}
//   <out_dir>/web/   {auto_emscripten.cpp, CMakeLists.txt, README.md}
//
// The trait carries the per-class config (target list, lib name, header
// basename) so the class definition stays pristine. Example:
//
//   template <> struct rosetta::binding_info<Person> {
//       static constexpr std::array  targets{"python", "node"};
//       static constexpr const char *lib    = "reflected_person";
//       static constexpr const char *header = "person.h";
//   };
//
// The user-include and rosetta-include directory paths are not in the
// trait — they are build-context-dependent and come from the caller
// (typically CLI flags on the driver).

#pragma once

#include <any>
#include <cstdio>
#include <experimental/meta>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <rosetta/annotations.h>
#include <rosetta/interop.h>
#include <rosetta/matrix.h>
#include <rosetta/sequence.h>
#include <rosetta/walk.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rosetta {

    /**
     * @brief Per-class binding configuration. Specialize this trait for each
     * type you pass to `rosetta::generate<Ts...>`. The class itself stays
     * unmodified; the only per-class metadata is its header basename.
     *
     * Required static member:
     *   - `header` — `const char*` basename used in `#include "..."`
     *
     * Optional static member:
     *   - `expose` — `const char*` binding name overriding the reflected
     *     identifier (manifest "expose"). Lets two classes that share an
     *     unqualified C++ name (arch::Data and arch::sinv::Data) coexist in
     *     one module: each is exposed — and its trampoline named — after
     *     this override, while the emitted C++ spells the class by its
     *     qualified name.
     *
     * The module / library name and the target backends are no longer
     * per-class — they live in `GenerateOptions` because one generator
     * call emits a single combined module per backend exposing every
     * class. Example:
     *   template <> struct rosetta::binding_info<Person> {
     *       static constexpr const char *header = "person.h";
     *   };
     */
    template <class T> struct binding_info; // primary — must be specialized

    /**
     * @brief One output target: a backend language and the module /
     * library name to bake into that backend's generated bindings.
     */
    struct TargetSpec {
        std::string lang; // "python", "node", "rest", "web"
        std::string name; // module / library name for this backend

        // Extra linker flags for THIS target only (manifest target
        // "link_options"). Per-target — unlike compile_definitions — because
        // link flags are inherently toolchain-specific: e.g. "-lnodefs.js"
        // is only meaningful for the wasm backends and would break a native
        // link.
        std::vector<std::string> link_options;

        // Where the BUILT ARTIFACT is copied after each build (manifest target
        // "out_dir"): the .so / .pyd / .node / .js+.wasm, not the generated
        // project tree. Absolute, and created if missing. Empty ⇒ the artifact
        // stays where the build put it (plus whatever next-to-the-sources
        // convenience copy the backend already makes).
        std::string artifact_dir;

        // Runtime pins for this target (manifest "python" / "requires_python" /
        // "napi_version" / "node_engine"). Empty ⇒ the generated project keeps
        // its defaults: `python3` off PATH, a 3.8 floor, N-API 8, and no
        // engines entry. `python` is a path or a bare version ("3.11", which
        // the emitted CMake spells python3.11).
        std::string python;
        std::string requires_python;
        std::string napi_version;
        std::string node_engine;
    };

    /**
     * @brief One external library the generated bindings link against
     * (manifest "user_lib"). Use these when the bound headers only *declare*
     * the API and the bodies live in separately-compiled libraries — your own
     * library plus, typically, the third-party ones it depends on.
     *
     *   name — the library's base name (e.g. "space" ⇒ -lspace,
     *          libspace.dylib / .so / .a).
     *   dir  — directory holding the built library (-L / rpath).
     *   link — preferred link form: "shared" (default) or "static". The
     *          generated CMake links that form by full path and falls back to
     *          whichever is actually present on disk. WebAssembly ignores it
     *          and always links static (a native shared object cannot enter a
     *          wasm module).
     *
     * Order is preserved on the link line, so list dependents before their
     * dependencies when linking static archives.
     */
    struct UserLib {
        std::string name;
        std::string dir;
        std::string link; // "shared" (default) | "static"; empty ⇒ shared
    };

    /**
     * @brief A reflected type, reduced to a small language-neutral descriptor
     * so pure-data backends (TypeScript, JSON Schema, …) can render it without
     * reflection. `kind` is one of: "number", "boolean", "string", "void",
     * "vector", "object", "enum", "unknown". For "vector", `element` holds one
     * entry (the element type); for "object" and "enum", `object` is the
     * class / enumeration identifier.
     */
    /** @brief One enumerator: its name and its value as a signed integer. */
    struct GenEnumerator {
        std::string name;
        long long   value = 0;
    };

    struct GenType {
        std::string          kind    = "unknown";
        std::string          object;  // class / enum identifier ("object" / "enum")

        // Namespace-qualified spelling of `object` ("arch::sinv::Data"; equal
        // to `object` for a global type). `object` alone cannot tell two bound
        // classes apart once they share an unqualified name (the "expose"
        // rename feature), so emitters that spell the type in C++ or resolve
        // it against the bound-class list use this instead. Empty only when
        // `object` is empty.
        std::string          object_qualified;
        std::vector<GenType> element; // 0 or 1 entry, the element when "vector"
        bool                 integer = false; // kind == "number" and integral (vs floating)
        std::string          spelling; // prettified C++ type spelling (for human docs)
        std::vector<GenEnumerator> enumerators; // populated when kind == "enum"

        // True when the type is a raw pointer to a class (`T*`); `object` then
        // holds the pointee class identifier. `kind` is deliberately left
        // "unknown" so backends that don't opt in keep skipping raw pointers (no
        // regression); a backend that CAN marshal a pointer to a bound class
        // (e.g. embind via allow_raw_pointers) checks this flag explicitly.
        bool is_pointer = false;

        // True when the (cvref-stripped) type is a std::shared_ptr<T>. Unlike
        // is_pointer / is_sequence / is_callback this does NOT change `kind`:
        // a shared_ptr is still described as the class it is ("object"), so
        // every backend keeps binding it exactly as before — nanobind, embind
        // and sol2 all marshal one through their own caster once the matching
        // header is included. The flag exists for the backends that must
        // additionally declare a HOLDER for the pointee: pybind11 refuses at
        // RUNTIME ("Unable to convert std::shared_ptr<T> to Python when the
        // bound type does not use std::shared_ptr ... as its holder type")
        // unless the pointee was registered as py::class_<T, std::shared_ptr<T>>.
        // `element` holds one entry, the pointee's descriptor (like "vector"),
        // so an emitter can resolve it against the bound-class list.
        bool is_shared_ptr = false;

        // True when the (cvref-stripped) type is a std::filesystem::path. `kind`
        // IS set to "string" — unlike the flags above, because a path *is* a
        // string to every host language, and every backend already marshals a
        // string. What the flag adds is the C++ side of that boundary: a path is
        // not a std::string, so the emitted code speaks std::string at the edge
        // and converts (`path{s}` in, `.string()` out). It rides the same copy
        // adapter as the foreign containers — see is_adapted() — which is why
        // the expanded backends needed no per-backend work for it. `element`
        // stays empty: there is nothing inside a path to describe.
        bool is_path = false;

        // True when the (cvref-stripped) type is a trait-registered foreign
        // sequence container (rosetta::is_sequence<T>, e.g. GEO::vector<double>
        // — see rosetta/sequence.h). Like is_pointer, `kind` stays "unknown" so
        // backends that don't opt in keep skipping it; an opted-in backend
        // marshals it by COPY through a std::vector<element> at the boundary.
        // `element` holds one entry (the element type, like kind == "vector")
        // and `seq_cpp` the qualified C++ spelling an emitted adapter can
        // construct ("GEO::vector<double>") — needed because display_string_of
        // prints template names unqualified.
        bool        is_sequence = false;
        std::string seq_cpp;

        // True when the (cvref-stripped) type is a trait-registered foreign
        // 2-D matrix (rosetta::is_matrix<T>, e.g. Eigen::MatrixXd — see
        // rosetta/matrix.h). Exactly the is_sequence contract one dimension up:
        // `kind` stays "unknown", `element` holds the (arithmetic) element and
        // `mat_cpp` the qualified spelling an adapter can construct, and an
        // opted-in backend marshals it by COPY through a
        // std::vector<std::vector<element>> — an array of rows, row-major
        // whatever the matrix's own storage order is, since operator()(i, j) is
        // the only access the registration promises.
        bool        is_matrix = false;
        std::string mat_cpp;

        // Non-empty when the type belongs to a foreign library the manifest
        // opted into ("interop": ["eigen"] — see rosetta/interop.h); holds that
        // library's manifest name ("eigen"). Like is_pointer / is_sequence,
        // `kind` stays "unknown", so a backend with no caster for the library
        // keeps skipping the member — deliberately, since binding it would
        // produce a call that always throws. A backend that CAN marshal it
        // (python / nanobind, once the caster header is
        // included) checks this flag and binds the type as it stands: no
        // adapter and no copy, because the caster owns the conversion.
        // `object` / `object_qualified` are still filled, so the exact
        // spellings (ret_cpp / param_cpp) re-qualify through qualify_objects
        // exactly like a bound class's.
        std::string interop;

        // True when the type is a std::function<R(A...)>. Like is_pointer, `kind`
        // stays "unknown" so backends that don't opt in keep skipping callbacks;
        // a backend that CAN marshal a JS function into a std::function (e.g.
        // embind via emscripten::val) checks this flag and consults `callback_sig`
        // to decide whether the whole signature is convertible. `callback_sig[0]`
        // is the return type (kind "void" when none); [1..] are the parameter
        // types, each cvref-stripped like any other GenType.
        bool                 is_callback = false;
        std::vector<GenType> callback_sig;

        // Copyability of the (cvref-stripped) type, captured at reflection time.
        // Every runtime backend copies at some boundary — pybind's automatic
        // return policy copies an lvalue-ref return, embind's property getters
        // and N-API's to_napi copy by construction/assignment — so emitters
        // consult these to SKIP (or downgrade to read-only) what would otherwise
        // be a hard compile error in the generated code. A class whose data API
        // lives in non-copyable public members (e.g. GEO::Mesh::vertices holding
        // a Mesh& back-reference) then binds cleanly as an opaque handle instead
        // of breaking the build. True for non-class kinds; false when the type
        // is incomplete at reflection time.
        bool copy_constructible = true;
        bool copy_assignable    = true;
    };

    /** @brief A numeric range constraint (rosetta::range annotation). */
    struct GenRange {
        bool   has = false;
        double min = 0;
        double max = 0;
    };

    /**
     * @brief One member the reflection walk dropped before any backend saw it,
     * as plain data for the coverage report. `reason` is a stable slug from
     * rosetta::drop_reason_name() ("no_identifier", "function_template",
     * "hidden_by_derived"); `signature` is the member's C++ type spelling, which
     * is what tells two hidden overloads apart.
     */
    struct GenDrop {
        std::string member;
        std::string signature;
        std::string reason;
    };

    struct GenField {
        std::string name;
        GenType     type;
        bool        is_readonly = false;
        std::string doc;     // rosetta::doc annotation text, if any
        GenRange    range;   // rosetta::range, if any
        std::vector<std::string> choices;       // rosetta::combobox choices, if any
        std::string              default_value; // default member initializer, rendered (if capturable)

        // Every annotation on this member, type-erased. Backends query the ones
        // they care about via find_annotation<A>() — the core names none of them.
        std::vector<std::any> annotations;
    };

    struct GenParam {
        // The parameter's own identifier, read from the declaration by
        // std::meta::identifier_of. Falls back to a synthesized "argN" for a
        // parameter declared without a name (`void f(double, int n)`) and for
        // the overload-selection path, which decomposes a function TYPE and so
        // has no declaration to read names from (see params_from_types).
        //
        // This is METADATA, not a C++ identifier: every backend that emits code
        // spells its own positional locals ("arg0", "arg1"), so a name that
        // happens to collide with a generated local, or with a keyword of the
        // target language, cannot break the emitted C++. Backends that render
        // the name into a HOST-language signature (TypeScript, C#, Java) run it
        // through that language's keyword guard first — see ts_param_name and
        // friends.
        std::string name;
        GenType     type;

        // Per-parameter documentation, harvested from the declaration's Doxygen
        // `@param <name> ...` block by rosetta_gen and applied after the walk
        // (GenerateOptions::doc_comments). Empty when the source carries none.
        std::string doc;

        // True when the C++ declaration gives this parameter a default argument
        // (std::meta::has_default_argument). Reflection reports the FACT but not
        // the expression, so a backend that only needs optionality — a
        // TypeScript `x?: number`, an OpenAPI `required: false` — is served by
        // this alone.
        bool has_default = false;

        // The default argument's source spelling ("32", "true", "Mode::Fast"),
        // harvested textually by rosetta_gen because reflection cannot yield it.
        // Empty when unavailable — the harvester is deliberately conservative and
        // refuses anything it cannot re-emit verbatim into the generated C++
        // (see doccomments.cpp). Only ever set when has_default is also true.
        std::string default_text;

        // True when the parameter is declared as an lvalue reference (T& /
        // const T&). A by-reference class parameter never copies — every
        // runtime backend hands the wrapped object through — so it is bindable
        // even for a non-copyable class; a by-VALUE class parameter copies and
        // needs type.copy_constructible.
        bool is_ref = false;

        // True for a NON-const lvalue reference. For class kinds that's the
        // mutable-receiver feature — the object crosses as a handle and the
        // callee writes through it. For everything else (std::string&,
        // index_t&, GEO::vector<double>&) the runtimes cannot bind their
        // converted argument, which is a temporary, so such a member is skipped
        // unless the manifest marks the parameter `out` (below).
        bool is_mutable_ref = false;

        // Marked by the manifest's "out_params" as a value the function
        // RETURNS through a reference. Never inferred: `void assign_points(
        // vector<double>&, index_t, bool)` takes its vector as an input it may
        // steal from, and `bool get_doubles(const string&, vector<double>&,
        // index_t&)` fills its vector as an output — the two are
        // indistinguishable in C++, and guessing wrong silently drops either an
        // argument or a result. The manifest knows; the walk cannot.
        //
        // A marked parameter disappears from the exposed signature and its
        // value joins the return: a tuple in Python, extra return values in
        // Lua, an array element in JS.
        bool is_out = false;
    };

    struct GenMethod {
        std::string            name;
        bool                   is_static = false;
        GenType                ret;
        std::vector<GenParam>  params;
        std::string            doc; // rosetta::doc annotation text, if any

        // The `@return` / `@returns` text of the declaration's Doxygen block,
        // harvested by rosetta_gen (GenerateOptions::doc_comments). Kept apart
        // from `doc` because the targets that can place it — a `:returns:` line
        // in a Python docstring, a `@returns` tag in TSDoc, the response
        // `description` of an OpenAPI operation — each want it on its own.
        std::string            returns;

        // Virtual / trampoline metadata, captured from the rosetta::virtual_spec
        // that walk<T>() synthesizes plus direct reflection queries. Used by
        // backends that emit overridable bindings (e.g. pybind11 trampolines).
        // `ret_cpp` / `param_cpp` are the *exact* C++ spellings (cv- and
        // ref-qualifiers preserved), unlike GenType::spelling which is
        // cvref-stripped for human docs — a trampoline override must match the
        // base signature exactly to actually override it.
        bool                     is_virtual  = false;
        bool                     is_pure     = false;
        bool                     is_const    = false;
        bool                     is_noexcept = false;
        std::string              ret_cpp;   // exact return-type spelling
        std::vector<std::string> param_cpp; // exact parameter-type spellings, in order

        // False when the return type or a parameter is something pybind11 has no
        // type-caster for in this TU (a pointer/vector-of-pointer to an incomplete
        // type, a raw C array). Computed at reflection time so a backend can skip
        // emitting a trampoline override it could not compile. Defaults true.
        bool sig_bindable = true;

        // True when the method returns an lvalue reference. Combined with
        // ret.copy_constructible this lets an emitter skip methods it would
        // otherwise fail to compile (pybind's automatic policy COPIES an
        // lvalue-ref return, e.g. GEO::Mesh::get_subelements_by_index()
        // returning a non-copyable store&).
        bool ret_is_ref = false;

        // True when the DECLARING C++ CLASS has more than one member function
        // with this name — a property of the source, not of the IR. The bare
        // member pointer `&T::name` is ambiguous for such a set, so an emitter
        // that spells one must disambiguate with an explicit static_cast to
        // this entry's exact signature (ret_cpp / param_cpp / is_const /
        // is_noexcept carry it). Note this stays true even when gating left a
        // single entry of the set in the IR: the ambiguity is in the C++, so
        // the cast is still required.
        bool is_overloaded = false;

        // Position of this entry within the IR's set of same-named methods, in
        // declaration order, and the size of that set. Unlike `is_overloaded`
        // these describe what actually REACHED the IR, which is what a backend
        // needs to act: `overload_count > 1` means the target language sees a
        // name collision it must resolve.
        //
        // A backend whose framework dispatches on argument types (pybind11,
        // nanobind, jlcxx) binds every entry and ignores both. One that keys
        // methods by name (embind, N-API, the C#/Java/REST op tables) can only
        // register a name once, and uses `overload_index == 0` to keep the
        // first-declared entry — see overload_policy in rosetta/coverage.h,
        // which also records the dropped siblings for the coverage report.
        std::size_t overload_index = 0;
        std::size_t overload_count = 1;

        // Extension method (manifest class "extensions"): a free function whose
        // first parameter is `Cls&`, exposed as an instance method of Cls.
        // `params` holds the parameters AFTER the receiver; `ext_qualified` is
        // the fully-qualified C++ spelling for &fn; `ext_header` its #include.
        // Backends that can only emit member pointers skip these.
        bool        is_extension = false;
        std::string ext_qualified;
        std::string ext_header;

        // Every annotation on this method, type-erased (mirrors GenField). UI
        // backends query the ones they care about — e.g. rosetta::button /
        // rosetta::label — via find_annotation<A>(); the core names none of them.
        std::vector<std::any> annotations;
    };

    /**
     * @brief One free (non-member) function, erased to plain data. Declared in
     * the manifest (header + name + optional doc) rather than reflected from a
     * type, so the user's headers stay pristine. `qualified` is the C++ spelling
     * a backend emits for the function pointer (e.g. `api::add`); `name` is the
     * unqualified identifier used as the exposed binding name.
     */
    struct GenFunction {
        std::string           name;      // exposed (unqualified) identifier
        std::string           qualified; // fully-qualified C++ spelling for &fn
        std::string           header;    // basename for #include
        GenType               ret;
        std::vector<GenParam> params;
        std::string           doc;     // from the manifest, or harvested from the header
        std::string           returns; // harvested `@return` text — see GenMethod::returns

        // Non-empty when the manifest picked ONE overload of an overloaded free
        // function by spelling its signature ("signature": "void(Mesh&, bool)").
        // The C++ function type exactly as the manifest gave it, which is what a
        // backend needs for the disambiguating cast — `&GEO::mesh_union` names an
        // overload SET and is ambiguous wherever a function pointer is expected.
        // Use gen_detail::fn_addr(), which spells `&qualified` when this is empty
        // and `static_cast<Ret(*)(Params)>(&qualified)` when it is not.
        //
        // It also flags what a backend CANNOT do: an emitter that splices the
        // function's reflection (`^^qualified`, the thin python / node / julia /
        // C# / Java / REST backends) has no way to name one member of an overload
        // set — `^^name` is ill-formed for it — so those skip such an entry.
        std::string sig_cpp;
    };

    /**
     * @brief An extension method (manifest class "extensions"): a free function
     * whose first parameter is `Cls&` (or `const Cls&`), exposed as an instance
     * method of the bound class `cls`. This is how a library whose own members
     * can't cross the boundary (raw-pointer accessors, attribute templates,
     * overloaded helpers) gets a scriptable surface WITHOUT a hand-written
     * wrapper class: the glue shrinks to stateless free functions and the
     * scripts keep holding the real C++ objects.
     */
    struct GenExtension {
        std::string cls; // the bound class, as spelled in the manifest ("GEO::Mesh")
        GenFunction fn;  // the free function (first param = receiver)
    };

    /**
     * @brief One class, erased to the plain data a backend needs. `generate`
     * fills this up front (the only place reflection runs), so backends are
     * pure text templating. Member type info (`fields` / `methods` / `ctors`)
     * is populated for pure-data backends; backends that emit C++ and defer to
     * a runtime visitor can ignore it and use just `name` / `header`.
     */
    struct GenClass {
        std::string name;       // reflected (unqualified) C++ identifier
        std::string name_space; // enclosing namespace ("" if global, "a::b" if nested)

        // Fully qualified C++ spelling: enclosing namespaces AND enclosing
        // classes ("sift::ParsedModel::TemporalDeriv"). Same role — and same
        // reason — as GenEnum::qualified: `class_namespace<T>()` stops at the
        // first non-namespace scope, so a class nested inside another class
        // reports an EMPTY `name_space` and the `name_space::name`
        // reconstruction collapses to the bare identifier. That is not a name
        // the emitted code can resolve: backends open namespaces with
        // `using namespace`, and cannot open a class. Read it through
        // qualified_of(), which falls back to the old
        // reconstruction when this is empty (hand-built IR).
        std::string qualified;

        // The name the class binds under (module attribute, JS export,
        // TypeScript class, trampoline suffix). Defaults to `name`; overridden
        // by binding_info<T>::expose (manifest "expose") so two classes with
        // the same unqualified C++ name can coexist in one module. Emitters
        // use this for every host-language-visible name and the qualified
        // `name_space::name` for every C++ spelling.
        std::string expose;
        std::string header; // binding_info<T>::header — basename for #include

        // Qualified names of the direct *public* base classes (e.g.
        // "arch::BaseRemote"), in declaration order. A backend that registers an
        // inheritance relationship — e.g. pybind11's py::class_<T, Base> so a
        // derived instance is accepted where a base pointer/reference is expected
        // — consults these, filtering to the bases that are themselves bound.
        std::vector<std::string> bases;
        std::string doc;    // class_markdown(*this) — per-class Markdown fragment (README body)

        // The class's OWN documentation — the comment written above it in the
        // header, harvested by rosetta_gen. Distinct from `doc`, which is a
        // rendered Markdown fragment listing the members: this is the one or two
        // sentences that belong in a `py::class_<T>(m, "T", <here>)`, at the top
        // of a TypeScript class, or as an OpenAPI schema description. Empty
        // unless the source carried a doc comment for the type.
        std::string brief;
        std::string annotations_json; // raw out-of-line annotation side-car (ann_json_source<T>), if any

        // Every class-level annotation, type-erased (see GenField::annotations).
        std::vector<std::any> annotations;

        std::vector<GenField>              fields;  // public data members
        std::vector<GenMethod>             methods; // instance + static methods
        std::vector<std::vector<GenParam>> ctors;   // one param list per constructor

        // Whether T is default-constructible. The implicitly-declared default
        // ctor is often *not* enumerated as a member, so `ctors` may be empty
        // even when `T()` is valid; backends that emit an explicit binding for
        // it (e.g. python's py::init<>()) consult this instead.
        bool is_default_constructible = false;

        // Whether T is abstract (has an unoverridden pure virtual). An abstract
        // class cannot be instantiated, so a backend must not emit any constructor
        // binding for it (embind's class_ constructor, for one, would try to
        // allocate the abstract type and fail to compile).
        bool is_abstract = false;

        // Whether T's destructor is PUBLIC. A ref-counted class hides it
        // (GEO::Logger derives from Counted and protects ~Logger), and every
        // runtime backend destroys what it wraps somewhere — pybind's holder,
        // node's `delete ptr_`, sol2's usertype — so binding one is a hard
        // COMPILE error deep in the framework's headers, not a runtime
        // surprise. Emitters skip such a class outright, with a note: the
        // alternative would be a per-backend non-owning holder
        // (py::nodelete and its equivalents), which is a feature of its own.
        bool is_destructible = true;

        // Whether T can be assigned to (copy OR move). The node runtime's
        // parameterized-constructor path assigns the freshly built object into
        // the Wrap's inner storage; for a non-assignable class (GEO::Mesh) only
        // the default constructor is emitted there.
        bool copy_or_move_assignable = true;

        // Whether T can be constructed from another T (copy OR move). The node
        // runtime's path for a NON-default-constructible class (e.g. a data
        // class whose only ctor is parameterized) builds the object straight
        // from the ctor_table entry into fresh storage, moving or copying the
        // returned value; the emitter registers no entries otherwise.
        bool copy_or_move_constructible = true;

        // Manifest "final": true — treat the class as non-overridable from the
        // host language: NO trampoline is generated even when it has public
        // virtual methods (they still bind as ordinary callable methods).
        // Beyond skipping useless shims, this is what lets the node runtime
        // hand the class out as an aliased member-object property — the alias
        // stores a T*, which requires the wrapped type to BE T, not Js_T
        // (GEO::MeshVertices, whose delete_elements/permute_elements virtuals
        // nobody script-overrides, is the motivating case).
        bool is_final = false;

        // Exact C++ spellings of each constructor's parameter types, in the same
        // order as `ctors`. Parallel to `ctors` (which carries the neutral IR);
        // a backend that has to *spell* the constructor signature in emitted C++
        // (e.g. py::init<const std::vector<double>&, ...>()) uses these, since
        // GenType::spelling is cvref-stripped and may not round-trip.
        std::vector<std::vector<std::string>> ctor_param_cpp;

        // Members the reflection walk could not hand to any backend — operators
        // and conversion functions (no bindable name), member templates, and
        // base overloads hidden by a derived declaration. These never become
        // GenMethods, so a backend cannot report them and their absence is
        // otherwise invisible; the coverage report reads them from here. Purely
        // informational: nothing in the binding path consults it.
        std::vector<GenDrop> dropped;
    };

    /**
     * @brief One enumeration, erased to plain data. Filled by `generate` (the
     * only place reflection runs) when a pack element is an enum type, so
     * backends render enums as pure text — no reflection.
     */
    struct GenEnum {
        std::string                name;       // reflected (unqualified) C++ identifier
        std::string                name_space; // enclosing namespace ("" if global, "a::b" if nested)

        // Fully qualified C++ spelling: enclosing namespaces AND enclosing
        // classes ("lookup::implicit3d::Modeler3D::SolverMode"). Distinct from
        // `name_space::name`, which stops at the first non-namespace scope and
        // therefore loses the owner of a *nested* enum — `name_space` is empty
        // for one, leaving the bare identifier, which does not compile. Only
        // this field is safe to emit as a C++ type spelling; `name_space` stays
        // namespace-only because backends turn it into `using namespace`.
        // Empty for hand-built IR (tests, direct render() callers), in which
        // case emitters fall back to `name_space::name`.
        std::string                qualified;

        // The name the enumeration binds under — binding_info<T>::expose
        // (manifest "expose") when set, else `name`. Same role as
        // GenClass::expose; emitters use it for every host-language-visible
        // name and the qualified `name_space::name` for C++ spellings.
        std::string                expose;
        std::string                header;     // binding_info<T>::header
        std::string                doc;        // markdown fragment for READMEs
        std::string                underlying; // underlying integer type spelling
        std::vector<GenEnumerator> values;     // enumerators in declaration order
    };

    /**
     * @brief One header written into the generated tree before anything
     * compiles (manifest "generated_headers"). `path` is relative — it is what
     * the bound sources #include ("geogram/version.h"), so the directory it
     * lands in goes FIRST on the include path, ahead of the library's own
     * source tree.
     */
    struct GeneratedHeader {
        std::string path;    // relative include path, e.g. "geogram/version.h"
        std::string content; // the finished text, substitutions already applied
    };

    /**
     * @brief One parameter of a harvested declaration: the name as the header
     * spells it, its `@param` text, and the default argument's source spelling.
     * `name` is what the match against the reflected signature keys on — the
     * cross-check that keeps a mis-parsed declaration from writing another
     * method's documentation into this one.
     */
    struct DocParam {
        std::string name;
        std::string doc;
        std::string default_text; // "" when absent or not safely re-emittable
    };

    /**
     * @brief One declaration's documentation, as rosetta_gen read it out of the
     * header: the Doxygen comment attached to it, plus what the declaration
     * itself says about its parameters. Purely descriptive — nothing here can
     * add, remove or rename a binding; it only fills text that reflection does
     * not carry (comments) and text it cannot carry (default arguments).
     */
    struct DocEntry {
        std::string           doc;     // brief + detail, already rendered
        std::string           returns; // `@return` / `@returns` text
        std::vector<DocParam> params;  // in declaration order
        bool                  is_function = false; // false for a field
    };

    struct GenerateOptions {
        std::filesystem::path    out_dir;         // root of the generated tree
        std::vector<std::filesystem::path> user_include; // dir(s) containing the class headers
        std::filesystem::path    rosetta_include; // path to rosetta's include/
        std::vector<TargetSpec>  targets;         // backends + per-backend module name
        std::vector<GenFunction> functions;       // free functions to expose
        std::vector<GenExtension> extensions;     // free functions exposed as class methods

        // Manifest "out_params": which parameters of which methods are
        // OUTPUTS. Keyed "Class::method" (or "ns::fn" for a free function),
        // each holding 0-based parameter indices. Applied to the IR after the
        // walk, since it is knowledge the C++ does not carry — see
        // GenParam::is_out.
        std::map<std::string, std::vector<std::size_t>> out_params;

        // Doc comments harvested from the user's headers by rosetta_gen (see
        // DocEntry). Keyed exactly like out_params — "Class::member" for a
        // field or method, "ns::fn" for a free function, and the bare class
        // spelling for the class's own comment. The value is a LIST because a
        // name can be overloaded; the entry is matched to an IR method by
        // arity and parameter names, and a group that cannot be told apart
        // contributes only what all its members agree on (see apply_doc_comments).
        //
        // This travels as generator options rather than through the annotation
        // side-car because it is per-PARAMETER data — a name, a doc string and a
        // default-argument spelling per index — which the annotation schema
        // (one value per member) has no way to express.
        std::map<std::string, std::vector<DocEntry>> doc_comments;

        // Manifest "module_init" — see GenContext::init_headers /
        // init_statements, which these fill verbatim.
        std::vector<std::string> init_headers;
        std::vector<std::string> init_statements;

        // Manifest "generated_headers": headers that do not exist on disk and
        // must be written before the bindings compile. The canonical case is a
        // header the bound library's OWN build system generates from a template
        // — geogram's <geogram/version.h>, configured from version.h.in — which
        // is simply absent when rosetta compiles that library's sources without
        // running its CMake. `content` is already resolved (rosetta_gen reads
        // the template and applies the substitutions), so generate() only has
        // to write it and put its directory first on the include path.
        std::vector<GeneratedHeader> generated_headers;

        // Class names (as spelled in the manifest, qualified or not) to mark
        // is_final — no trampoline, host-language overriding off. See
        // GenClass::is_final.
        std::vector<std::string> final_classes;

        // Optional pointers to the C++26 / P2996 reflection toolchain, baked into
        // the *thin* backends' generated CMakeLists so reflection-driven targets
        // find the right compiler and runtime without editing the output. The
        // stock *-expanded targets ignore all of these. Each is also overridable
        // at configure time (-DCLANG_P2996_ROOT=..., -DROSETTA_CXX_COMPILER=...,
        // -DROSETTA_C_COMPILER=..., -DROSETTA_STDLIB=...).
        //
        //   cpp26_root — toolchain root (clang-p2996 build dir). Empty ⇒ built-in
        //                default $ENV{HOME}/devs/c++/clang-p2996/build. The three
        //                below default to ${CLANG_P2996_ROOT}/{bin/clang++,
        //                bin/clang,lib} when empty, so usually only this is set.
        //   cpp26_cxx  — C++ compiler (name or full path).
        //   cpp26_cc   — C compiler (name or full path).
        //   cpp26_lib  — directory holding the fork's libc++ / libc++abi, used for
        //                -L and -rpath (the "lib" the binding links against).
        std::string cpp26_root;
        std::string cpp26_cxx;
        std::string cpp26_cc;
        std::string cpp26_lib;

        // Optional Qt 6 install prefix, baked as the default of the QT_DIR cache
        // variable in the qt / qml CMakeLists. Empty ⇒ built-in default
        // ($ENV{HOME}/Qt/6.8.3/macos). Overridable at configure time with
        // -DQT_DIR=...; backends other than qt/qml ignore it.
        std::string qt_dir;

        // Optional external user libraries to link the generated bindings
        // against (manifest "user_lib"). Use these when the bound headers only
        // *declare* the API and the bodies live in separately-compiled (shared
        // or static) libraries — the binding TU then needs them at link time.
        // The stock *-expanded backends emit one target_link_directories /
        // target_link_libraries (+ rpath) pair per entry, in order, so a
        // library and every dependency it needs can be listed together.
        // See UserLib for the per-entry fields.
        std::vector<UserLib> user_libs;

        // Single-library shorthand, kept for hand-written drivers predating
        // `user_libs`: when `user_libs` is empty and `user_lib_name` is set,
        // generate() folds these three into one `user_libs` entry.
        std::string user_lib_name;
        std::string user_lib_dir;
        std::string user_lib_link; // "shared" (default) | "static"; empty ⇒ shared

        // Optional user source files (.cpp) compiled directly into every generated
        // binding target. Use this — instead of (or alongside) user_lib — when the
        // bound headers only *declare* the API and the bodies live in source files
        // you want built into the binding rather than linked from a pre-built
        // library. Each compiled backend adds them to its binding target via
        // target_sources(); the text-only backends ignore them. Absolute paths.
        // Entries may be C sources (.c) — the generated CMakeLists then calls
        // enable_language(C) so they build (vendored zlib / rply / libMeshb…).
        std::vector<std::filesystem::path> user_sources;

        // Optional preprocessor definitions applied to every compiled binding
        // target (and picked up by user_sources), each "NAME" or "NAME=VALUE" —
        // e.g. {"GEOGRAM_USE_BUILTIN_DEPS", "GEOGRAM_WITH_HLBFGS"}. Emitted as
        // target_compile_definitions(... PRIVATE ...); text-only backends
        // ignore them.
        std::vector<std::string> compile_definitions;

        // Optional build configuration baked into every compiled backend's
        // generated CMakeLists (text-only backends have none).
        //
        //   build_type   — default CMAKE_BUILD_TYPE ("Debug", "Release",
        //                  "RelWithDebInfo", "MinSizeRel"), emitted inside
        //                  if(NOT CMAKE_BUILD_TYPE AND NOT
        //                  CMAKE_CONFIGURATION_TYPES) so -DCMAKE_BUILD_TYPE=...
        //                  at configure time still wins and multi-config
        //                  generators are left alone. Empty ⇒ RELEASE. It used
        //                  to mean "emit nothing", which left CMake with no
        //                  build type and therefore no -O at all: the
        //                  documented two-line build shipped an unoptimized
        //                  module (measured 8x slower on a pybind11 call).
        //                  A binding is a redistributable artifact, so the
        //                  no-op default was a footgun rather than neutral.
        //   optimization — explicit optimization flag ("-O0".."-O3", "-Os",
        //                  "-Oz", "-Og", "-Ofast") added via
        //                  add_compile_options / add_link_options, which land
        //                  AFTER the build type's per-config flags on the
        //                  command line — so this -O overrides the build
        //                  type's own level. Empty ⇒ not emitted.
        std::string build_type;
        std::string optimization;

        // Optional C++ standard the user_sources compile with ("17", "20",
        // "23", "26"). Use e.g. "17" when the user sources (or their vendored
        // third parties) are not C++20-clean. Emitted as per-source
        // COMPILE_OPTIONS "-std=c++NN", which lands after the target's own
        // standard flag and so wins for those files only — the generated
        // binding TU keeps its backend's standard (C++20 expanded, C++26
        // thin), which its runtime headers require. Empty (or "20") ⇒ no
        // per-source flag. C sources (.c) are never touched.
        std::string cxx_standard;

        // Optional distribution version (manifest "version"), a PEP 440 /
        // semver string ("1.2.0", "0.3.0rc1"). Only the packaging artifacts
        // consume it — the pyproject.toml the python / nanobind-
        // expanded backends emit for wheel builds. Empty ⇒ the backends fall
        // back to DEFAULT_DIST_VERSION ("0.1.0"), so a manifest that never
        // packages needs no change.
        std::string version;
    };

    /**
     * @brief Everything a backend needs to emit one target's project tree.
     */
    struct GenContext {
        std::filesystem::path    out_dir;         // root of the generated tree
        std::string              lib;             // this target's module / library name
        std::vector<GenClass>    classes;         // all classes to expose
        std::vector<GenEnum>     enums;           // all enumerations to expose
        std::vector<GenFunction> functions;       // all free functions to expose
        std::string              user_include;    // dir containing the class headers
        std::string              rosetta_include; // path to rosetta's include/
        std::string              cpp26_root;      // C++26 toolchain root (default of CLANG_P2996_ROOT)
        std::string              cpp26_cxx;       // C++ compiler   (default ${CLANG_P2996_ROOT}/bin/clang++)
        std::string              cpp26_cc;        // C compiler     (default ${CLANG_P2996_ROOT}/bin/clang)
        std::string              cpp26_lib;       // fork stdlib dir (default ${CLANG_P2996_ROOT}/lib)
        std::string              qt_dir;          // Qt 6 prefix (default of QT_DIR; qt/qml backends)
        std::vector<UserLib>     user_libs;       // external libs to link the bindings against, in link order
        std::vector<std::string> user_sources;    // user .cpp/.c files compiled into the binding target (abs paths)
        std::vector<std::string> compile_definitions; // "NAME"/"NAME=VALUE" defs for the binding target
        std::vector<std::string> link_options;    // extra linker flags for THIS target (TargetSpec::link_options)
        std::string              build_type;      // default CMAKE_BUILD_TYPE ("" ⇒ Release)
        std::string              optimization;    // explicit -O flag overriding the build type's ("" ⇒ not emitted)
        std::string              cxx_standard;    // per-source -std for the user sources ("" or "20" ⇒ none)
        std::string              version;         // distribution version for packaging ("" ⇒ DEFAULT_DIST_VERSION)

        // Foreign libraries this binding opted into (manifest "interop", e.g.
        // {"eigen"}) — the same names GenType::interop carries. A backend that
        // owns a caster for one emits its header (<pybind11/eigen.h>,
        // <nanobind/eigen/dense.h>); the others ignore the list, having already
        // skipped every member that names such a type. Filled from the traits
        // in the generated bindings.h, so it needs no GenerateOptions field.
        std::vector<std::string> interop;

        // Where this target's built artifact is copied after each build
        // (TargetSpec::artifact_dir / manifest target "out_dir"). Consumed by
        // render_meta's {{OUT_DIR_BLOCK}}, so a backend gets the behaviour by
        // placing that placeholder in its CMake template.
        std::string artifact_dir;

        // Runtime pins (TargetSpec::python and friends), consumed by
        // render_meta's {{PYTHON_CMD}} / {{PYTHON_MIN}} / {{REQUIRES_PYTHON}} /
        // {{NAPI_VERSION}} / {{NODE_ENGINES}}. Each has a default, so a
        // template names the placeholder unconditionally.
        std::string python;
        std::string requires_python;
        std::string napi_version;
        std::string node_engine;

        // Manifest "module_init": C++ statements to run when the module LOADS,
        // and the headers that declare them. Emitted at the top of the module
        // entry point — PYBIND11_MODULE / NB_MODULE / Init / EMSCRIPTEN_BINDINGS
        // / luaopen — before any binding is registered.
        //
        // This is the escape hatch for a library's LIFECYCLE, which is not a
        // binding at all: geogram wants GEO::initialize() plus a run of
        // CmdLine::import_arg_group() calls plus a C-function-pointer
        // registration (nlPrintfFuncs) before anything else works, and none of
        // that is expressible as "bind this name". Without it, every such
        // library needs a hand-written init function in the manifest's
        // `functions` that scripts must remember to call first.
        std::vector<std::string> init_headers;
        std::vector<std::string> init_statements;
    };

    /**
     * @name IR accessors
     * The vocabulary a backend uses to name a bound type. Two spellings that
     * are never interchangeable: `qualified_of` is the C++ one, for anything
     * the generated TU has to compile; `exposed_of` is the host-language one,
     * honoring the manifest's "expose" rename. `exposed_object_of` resolves
     * the type an IR entry *references* to its exposed name, which is what a
     * backend printing a cross-reference (a TypeScript class, a C# type, a doc
     * link) wants. Defined inline in inline/generate.hxx.
     *
     * Public because out-of-tree backends need them as much as the built-in
     * ones do — see docs/EXTENDING_BACKEND.md. They lived in
     * `rosetta::gen_detail` until 2026-08 and that spelling still resolves.
     * @{
     */
    std::string qualified_of(const GenClass &k);
    std::string exposed_of(const GenClass &k);
    std::string qualified_of(const GenEnum &e);
    std::string exposed_of(const GenEnum &e);
    /** @brief Does IR type `t` name the bound class / enumeration `k`? */
    template <typename K> bool names_type(const GenType &t, const K &k);
    std::string exposed_object_of(const GenType &t, const GenContext &c);
    /** @} */

    /**
     * @brief Code-generation backend for one target language. Implement this
     * and register it (see `register_backend`) to teach `generate` a new
     * backend — no edit to `generate` itself is required.
     */
    struct Backend {
        virtual ~Backend()                          = default;
        // Write this target's project tree under c.out_dir.
        virtual void emit(const GenContext &) const = 0;
        // Render this backend's primary document to a string, for single-artifact
        // ("document") backends like markdown / html. Multi-file project backends
        // (python, node, rest, …) have no single string and leave this empty.
        virtual std::string render(const GenContext &) const { return {}; }
    };

    /**
     * @brief The lang → backend map consulted by `generate` at run time.
     * Seeded with the built-in "python", "node", "rest", "web" backends on
     * first use.
     */
    std::map<std::string, std::shared_ptr<Backend>> &backend_registry();

    /** @brief Register (or override) the backend handling `lang`. */
    void register_backend(std::string lang, std::shared_ptr<Backend> backend);

    /**
     * @brief Static-init helper: declare one at namespace scope in a plugin
     * translation unit linked into the generator to register a backend before
     * `main` runs. e.g.
     *   static rosetta::BackendRegistrar lua{"lua", std::make_shared<LuaBackend>()};
     */
    struct BackendRegistrar {
        BackendRegistrar(std::string lang, std::shared_ptr<Backend> backend) {
            register_backend(std::move(lang), std::move(backend));
        }
    };

    /**
     * @brief Scaffold the per-backend binding projects under opt.out_dir for
     * the whole set of classes `Ts...`. The pack is erased into a
     * `std::vector<GenClass>` and each target is dispatched through
     * `backend_registry()`; this function never changes when a backend is
     * added. Per-class headers come from the `rosetta::binding_info<T>` trait.
     */
    template <typename... Ts> void generate(const GenerateOptions &opt);

    /**
     * @brief Describe one free function (identified by its reflection `F`) as
     * plain data for `GenerateOptions::functions`. The generated driver calls
     * this with `^^name` for each function listed in the manifest; `qualified`
     * is the C++ spelling backends emit for the function pointer and `header`
     * its include basename. Free functions are declared in the manifest, never
     * by editing the user's headers.
     *
     * `expose` (manifest "expose") overrides the binding name — what scripts
     * see — leaving `qualified` (the C++ spelling) alone; it is how two free
     * functions sharing an unqualified name (`arch::solve` and
     * `arch::sinv::solve`) coexist in one module. Empty / null ⇒ the function
     * binds under its own reflected identifier.
     */
    template <std::meta::info F>
    GenFunction make_function(const char *qualified, const char *header, const char *doc,
                              const char *expose = nullptr);

    /**
     * @brief Same, for ONE overload of an overloaded free function — selected by
     * its signature (manifest `"signature": "void(GEO::Mesh&, bool)"`) instead of
     * by its reflection.
     *
     * `^^name` is ill-formed when `name` is an overload set, so the reflection
     * path `make_function<^^F>` cannot express "this one". The signature can:
     * `Sig` is the function TYPE (`void(GEO::Mesh &, bool)`), a plain type
     * argument no overload set is involved in, and the return type / parameters
     * come from decomposing it rather than from reflecting a function. The
     * exposed name is `expose`, or the tail of `qualified` after the last `::`
     * (there is no `identifier_of` to ask).
     *
     * `sig_cpp` is that same signature as text, kept for the disambiguating cast
     * backends must emit — see GenFunction::sig_cpp.
     */
    template <typename Sig>
    GenFunction make_function_sig(const char *qualified, const char *header, const char *doc,
                                  const char *expose, const char *sig_cpp);

} // namespace rosetta

// The coverage vocabulary (rosetta::coverage::) sits between the IR structs it
// describes and inline/generate.hxx, whose generate() writes the report. See the
// ordering note at the top of coverage.h.
#include <rosetta/coverage.h>

#include "inline/generate.hxx"
