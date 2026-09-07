// Copyright (c) fmaerten@gmail.com
// License: MIT

// The manifest model: the structs a parsed manifest.json is loaded into,
// and load() itself (manifest.cpp).
//
// Manifest shape:
//   {
//     "user_include": "./geom",
//     "rosetta_include": "../../include",
//     "generator_name": "generator_geom",           // driver tool / CMake target
//     "module_name": "geom",                        // default binding module name
//     "variables": [                                // optional: manifest-local
//       {"name": "P2996",                           //   variables, substituted as
//        "value": "$ENV{HOME}/clang-p2996"}         //   $NAME / ${NAME} into every
//     ],                                            //   string of the manifest, so
//                                                   //   a shared prefix is written
//                                                   //   once. Ordered (a value may
//                                                   //   use the ones before it).
//                                                   //   $ENV{...} and any undeclared
//                                                   //   ${...} pass through to CMake
//                                                   //   untouched.
//     "cpp26_root": "$P2996/build",                 // optional: C++26/P2996
//                                                   // toolchain root for the thin
//                                                   // (reflection) backends. Default:
//                                                   // $ENV{HOME}/devs/c++/clang-p2996/build
//     "cpp26_cxx": "clang++",                       // optional: C++ compiler (name or
//                                                   //   path). Default ${root}/bin/clang++
//     "cpp26_cc":  "clang",                         // optional: C compiler.
//                                                   //   Default ${root}/bin/clang
//     "cpp26_lib": "/path/to/fork/lib",             // optional: dir with libc++/
//                                                   //   libc++abi (-L/-rpath).
//                                                   //   Default ${root}/lib
//     "qt_dir": "$ENV{HOME}/Qt/6.8.3/macos",        // optional: Qt 6 prefix for the
//                                                   //   the qt / qml backends.
//                                                   //   Default that path.
//     "build_type": "Release",                      // optional: default CMAKE_BUILD_TYPE
//                                                   //   baked into every compiled backend's
//                                                   //   CMakeLists (Debug | Release |
//                                                   //   RelWithDebInfo | MinSizeRel);
//                                                   //   -DCMAKE_BUILD_TYPE=... still wins
//     "optimization": "-O2",                        // optional: explicit -O flag added
//                                                   //   after the build type's own flags
//                                                   //   (so it wins): -O0..-O3, -Os, -Oz,
//                                                   //   -Og, -Ofast
//     "cxx_standard": 17,                           // optional: C++ standard the
//                                                   //   user_sources compile with
//                                                   //   (11|14|17|20|23|26, number or
//                                                   //   string) — per-source -std that
//                                                   //   wins over the target's. The
//                                                   //   generated binding TU keeps its
//                                                   //   backend's standard (C++20
//                                                   //   expanded / C++26 thin), which
//                                                   //   its runtime headers require.
//     "version": "1.2.0",                           // optional: distribution version for
//                                                   //   the packaging artifacts — the
//                                                   //   pyproject.toml emitted by the
//                                                   //   python-expanded / nanobind-expanded
//                                                   //   backends for wheel builds. PEP 440
//                                                   //   ("1.2.0", "0.3.0rc1"). Default 0.1.0
//     "interop": ["eigen"],                         // optional: foreign libraries the target's
//                                                   //   binding framework marshals itself. With
//                                                   //   "eigen", python-/nanobind-expanded bind
//                                                   //   Eigen types natively (numpy, no copy);
//                                                   //   backends with no caster skip those
//                                                   //   members instead of binding a call that
//                                                   //   throws. See rosetta/interop.h.
//     "user_lib": {                                 // optional: external library to link
//       "name": "space",                            //   the bindings against (libspace.*).
//       "dir":  "../space/bin",                     //   Use when the bound headers only
//       "link": "shared"                            //   declare the API and the bodies
//     },                                            //   live in a separately-compiled lib.
//                                                   //   `dir` is relative to the manifest.
//                                                   //   `link`: "shared" (default) | "static"
//                                                   //   | "dynamic" (alias of shared) — the
//                                                   //   preferred form, with fallback to
//                                                   //   whichever is built. wasm is always
//                                                   //   static (no native .so in wasm).
//                                                   //   May also be an ARRAY of such objects
//                                                   //   — the bound library plus the ones it
//                                                   //   depends on, linked in order:
//                                                   //   "user_lib": [
//                                                   //     {"name":"space","dir":"../space/bin"},
//                                                   //     {"name":"foo","dir":"/opt/foo/lib",
//                                                   //      "link":"static"} ]
//     "compile_definitions": [                      // optional: preprocessor definitions
//       "XXX_USE_BUILTIN_DEPS",                     //   ("NAME" or "NAME=VALUE") applied to
//       "XXX_WITH_HLBFGS"                           //   the driver AND every compiled
//     ],                                            //   binding target (they reach the bound
//                                                   //   headers and user_sources alike).
//     "namespace": "stressinv",                     // optional: default namespace for
//                                                   //   class/function/extension names
//                                                   //   without a "::" of their own
//                                                   //   ("Serie" -> "stressinv::Serie";
//                                                   //   qualified names pass verbatim,
//                                                   //   leading "::" = global namespace)
//     "header_dir": "stressinv",                    // optional: dir fragment prepended
//                                                   //   to every entry header
//                                                   //   ("Serie.h" -> "stressinv/Serie.h")
//                                                   // Both also appear on GROUPS inside
//                                                   //   "classes"/"functions": an object
//                                                   //   with "entries" (a nested list)
//                                                   //   scoping local defaults —
//                                                   //   namespace appends (":: " prefix
//                                                   //   = absolute), header_dir appends,
//                                                   //   optional "header" is the default
//                                                   //   for entries spelling none.
//                                                   //   Groups nest and mix with plain
//                                                   //   entries:
//                                                   //   { "header_dir": "solvers",
//                                                   //     "entries": [
//                                                   //       {"header": "Gmres.h"} ] }
//     "targets": [                                  // shared by every class
//       { "lang": "python", "name": "pygeom" },     // per-target module name
//       { "lang": "nanobind", "name": "nbgeom",     // optional per-target
//         "wheel": true,                            //   packaging: build a wheel
//         "wheel_dir": "./dist" },                  //   on every --build, into
//                                                   //   this dir (python /
//                                                   //   nanobind only)
//       { "lang": "wasm-expanded",                  // optional per-target linker
//         "link_options": ["-lnodefs.js"] },        //   flags (only THIS target's
//       "node"                                      //   link line — flags are
//     ],                                            //   toolchain-specific)
//     "classes": [
//       { "name": "Model", "header": "Model.h",
//         "annotations": "Model.ann.json",          // optional out-of-line annotations
//         "extensions": [                           // optional: free functions (first
//           { "name": "ext::vertices",              //   param `Model&`) exposed as
//             "header": "model_ext.h",              //   instance methods — glue for
//             "doc": "..." }                        //   members that can't cross the
//         ] },                                      //   boundary (raw ptrs, overloads)
//       { "header": "Point.h" },                    // name derived from header stem
//       { "header": "shapes/*.h",                   // a GLOB binds every header it
//         "exclude": ["shapes/detail_*.h"] }        //   matches, one class per file
//     ],                                            //   named from the file's stem
//                                                   //   ("**/*.h" recurses). Optional
//                                                   //   "exclude" drops patterns; a
//                                                   //   header (or name) an explicit
//                                                   //   entry claims is left to it.
//     "functions": [                                // optional: free (non-member) fns
//       { "name": "transform", "header": "common.h", "doc": "...",
//         "expose": "warp",                         // optional: binding name, overriding
//                                                   //   the identifier (same rule as a
//                                                   //   class's "expose"; also valid on
//                                                   //   an "extensions" entry). Name may
//                                                   //   be qualified (api::add).
//         "signature": "void(Point&, double)" }     // optional: bind ONE overload of an
//     ],                                            //   overloaded name — the only way in,
//                                                   //   since ^^name is ill-formed for an
//                                                   //   overload set.
//     "preset": "scriptable",                       // optional: a name, or a list of
//                                                   //   them. Pulls in the JSON fragment
//                                                   //   <rosetta_include>/rosetta/presets/
//                                                   //   <name>.json — a FIXED binding
//                                                   //   surface (rosetta's own reflection
//                                                   //   API, say), so the manifest states
//                                                   //   only what is project-specific.
//                                                   //   Deep-merged, manifest wins; its
//                                                   //   classes/functions are parsed with
//                                                   //   a pristine context and it puts
//                                                   //   rosetta_include on the target's
//                                                   //   include path.
//     "module_init": {                              // optional: statements the module
//       "headers": ["lib/lifecycle.h"],             //   runs when it LOADS, plus the
//       "statements": ["lib::initialize()"]         //   headers declaring them. The
//     },                                            //   library's lifecycle — not a
//                                                   //   binding. A bare array = the
//                                                   //   statements alone.
//     "generated_headers": [                        // optional: headers the bound
//       {"path": "geogram/version.h",               //   library's OWN build system
//        "template": "./ext/version.h.in",          //   generates. Written into the
//        "substitutions": {"VERSION": "1.10.1"}}    //   bindings tree and put FIRST on
//     ],                                            //   the include path. "content":
//                                                   //   [lines] instead of a template.
//     "sequences": [                                // optional: foreign sequence
//       "GEO::vector",                              //   containers (ONE type param) —
//       { "type": "Eigen::VectorXd" }               //   marshal like std::vector<T>.
//     ]                                             //   The object form registers a
//   }                                               //   CONCRETE type, spelled exactly
//                                                   //   (see rosetta/sequence.h)

#pragma once

#include "doccomments.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FunctionEntry {
    std::string name;   // (optionally qualified) C++ function name, e.g. "api::add"
    std::string header; // header declaring it
    std::string doc;    // optional manifest doc string

    // Optional "expose": the binding name, overriding the C++ identifier —
    // how two free functions sharing an unqualified name (arch::solve and
    // arch::sinv::solve) coexist in one module, and how an extension method
    // is renamed on the class it attaches to. Empty means the function binds
    // under its own (unqualified) identifier.
    std::string expose;

    // Optional "signature": the C++ function TYPE of the one overload to bind,
    // e.g. "void(GEO::Mesh&, const GEO::Mesh&, const GEO::Mesh&, bool)". Only
    // needed when `name` is overloaded — `^^name` is ill-formed for an overload
    // set, so the reflection path cannot say which one is meant; the signature
    // can, and it is what the generated driver passes to make_function_sig.
    // Empty ⇒ the ordinary `make_function<^^name>` path.
    std::string signature;
};

// One entry of the manifest's "sequences" / "matrices": a foreign container
// that marshals like a std::vector of its element (or, for a matrix, like a
// vector of row vectors). Two forms, because two shapes of container exist:
//
//   "GEO::vector"                 — a TEMPLATE with one type parameter; the
//                                   adapter spells it Namespace::Template<elem>
//   { "type": "Eigen::VectorXd" } — a CONCRETE type, spelled exactly as given.
//                                   Needed whenever the specialization carries
//                                   more than its element (VectorXd is really
//                                   Matrix<double, -1, 1>, which would compose
//                                   to the uncompilable Matrix<double>), and
//                                   the only way to register a non-template.
struct ContainerEntry {
    std::string name;          // "GEO::vector" | "Eigen::VectorXd"
    bool        exact = false; // true ⇒ concrete type (full specialization)
};

// The manifest's "module_init": C++ statements run when the generated module
// loads, plus the headers that declare them. A library's LIFECYCLE — geogram's
// GEO::initialize() + CmdLine::import_arg_group() run + nlPrintfFuncs() —
// is not a binding, and this is the only field that expresses it.
struct ModuleInit {
    std::vector<std::string> headers;
    std::vector<std::string> statements;
};

// One entry of the manifest's "generated_headers": a header the bound library's
// own build system would have produced, which rosetta must write itself because
// it compiles that library's sources without running its build. Either form
// resolves to the same finished text here — a template file with @KEY@
// placeholders plus substitutions, or literal lines.
struct GeneratedHeaderEntry {
    std::string path;    // relative include path ("geogram/version.h")
    std::string content; // resolved text, ready to write
};

struct ClassEntry {
    std::string name;
    std::string header;
    fs::path    annotations; // optional out-of-line annotation JSON (absolute); empty if none

    // Optional "expose": the binding name, overriding the C++ identifier —
    // how two classes sharing an unqualified name (arch::Data and
    // arch::sinv::Data) coexist in one module. Emitted as
    // binding_info<T>::expose; empty means the class binds under its own
    // (unqualified) identifier.
    std::string expose;

    // Optional "final": true — no trampoline even when the class has public
    // virtual methods (they still bind as callable methods; host-language
    // overriding is off). Also what makes the class eligible as a node
    // member-object property when it has virtuals (the alias stores a T*).
    bool final_ = false;

    // Optional extension methods ("extensions"): free functions whose first
    // parameter is `<name>&`, exposed as instance methods of the class. This
    // is the escape hatch for a library whose own members can't cross the
    // boundary (raw-pointer accessors, attribute templates, overloaded
    // helpers): the glue shrinks to stateless free functions — no wrapper
    // class — and scripts keep holding the real C++ objects.
    std::vector<FunctionEntry> extensions;
};

// One entry of the manifest's "user_lib": a pre-built library the generated
// bindings (and the generator driver) link against. Several may be listed —
// the bound library plus the ones it depends on — and they link in order.
struct UserLibEntry {
    std::string name; // library base name ("space" ⇒ libspace.dylib / .so / .a)
    std::string dir;  // absolute dir holding it (-L / rpath)
    std::string link; // "shared" (default) | "static"; wasm always static
};

struct TargetEntry {
    std::string lang; // "python", "node", "rest", "web"
    std::string name; // module / library name for this backend

    // Optional extra linker flags for THIS target only ("link_options").
    // Per-target — unlike compile_definitions — because link flags are
    // toolchain-specific: e.g. "-lnodefs.js" only makes sense on a wasm
    // target and would break a native link.
    std::vector<std::string> link_options;

    // Optional "out_dir": where the BUILT ARTIFACT is copied after each build
    // (the .so / .pyd / .node / .js+.wasm), absolute. This is not where the
    // generated project goes — that is the generator's own output tree — but
    // where the loadable module lands, so a Python package directory or a
    // web app's assets dir can be the destination and the binding needs no
    // copy step of its own. Empty ⇒ only the existing next-to-the-sources
    // convenience copy. Defaults to the manifest's top-level "out_dir".
    std::string out_dir;

    // Optional runtime pins, per target because two python targets in one
    // manifest may legitimately want different interpreters:
    //
    //   "python"          — the interpreter the binding is BUILT for, as a path
    //                       ("/opt/py311/bin/python3") or a bare version
    //                       ("3.11" ⇒ python3.11, resolved on PATH). Without
    //                       one the generated CMake probes `python3`, i.e.
    //                       whatever the PATH says — which is why a venv used
    //                       to be the only way to choose. Also the interpreter
    //                       `--build --wheel` runs make_wheel.py with, so the
    //                       wheel is tagged for the pinned version.
    //   "requires_python" — minimum version (">=3.10"), feeding BOTH the
    //                       find_package(Python …) floor and pyproject.toml's
    //                       requires-python, which cannot drift apart as a
    //                       result. Default ">=3.8".
    //   "napi_version"    — N-API version the addon targets (NAPI_VERSION=,
    //                       default 8). This is the real Node floor: N-API 8
    //                       means Node 12.22+, N-API 9 means Node 18.17+.
    //   "node_engine"     — package.json "engines.node" (">=18"), which is
    //                       documentation for npm rather than a compile
    //                       setting; kept separate from napi_version for that
    //                       reason, since only you know which you mean.
    std::string python;
    std::string requires_python;
    std::string napi_version;
    std::string node_engine;

    // Optional packaging, for the two backends that emit a make_wheel.py
    // (python / nanobind — any other lang carrying these is an error):
    //
    //   "wheel"     — build a wheel for THIS target on every --build, without
    //                 passing --wheel each time. A project that always ships
    //                 says so once. The flag still wins and only ever toward
    //                 packaging MORE, so a target saying false cannot disarm a
    //                 --wheel that was typed.
    //   "wheel_dir" — where this target's wheels go instead of its own dist/,
    //                 absolute. Implies `wheel`, exactly as --wheel-dir does.
    //                 --wheel-dir on the command line overrides it.
    //
    // Per-target rather than top-level because the two python backends
    // genuinely differ here: nanobind emits one abi3 wheel covering every
    // CPython 3.12+, pybind11 one wheel per version, and `python` above already
    // pins which interpreter each is tagged for. Sharing one output directory
    // is still safe — each make_wheel.py repairs only the wheels it produced.
    bool        wheel = false;
    std::string wheel_dir;
};

struct Manifest {
    std::vector<fs::path>      user_include;    // one or more, absolute
    fs::path                   rosetta_include; // absolute
    std::string                generator_name;  // driver tool / CMake target name
    std::vector<TargetEntry>   targets;         // backends + per-backend module name
    std::vector<ClassEntry>    classes;
    std::vector<FunctionEntry> functions; // free functions to expose
    ModuleInit                 module_init; // "module_init": load-time statements

    // "out_params": which parameters are OUTPUTS, keyed "Class::method" or
    // "ns::function", each a list of 0-based indices. Never inferred — see
    // GenParam::is_out for why the C++ cannot say.
    std::map<std::string, std::vector<std::size_t>> out_params;

    // "doc_comments": harvest the Doxygen documentation and default arguments
    // already present in the bound headers (default true — a library's own
    // comments are the best docstrings a generated binding can have, and they
    // cost nothing to read). Set false to generate from annotations alone.
    bool doc_comments = true;

    // The result of that harvest, keyed "Class::member" / "ns::fn" — see
    // doccomments.h. Filled by load(); baked into the driver by emit.cpp and
    // matched against the reflected signatures by rosetta::generate().
    DocMap harvested_docs;
    std::vector<GeneratedHeaderEntry> generated_headers; // "generated_headers"

    // Optional foreign sequence containers ("sequences"): qualified template
    // names with ONE type parameter ("GEO::vector"), or concrete types spelled
    // exactly ({ "type": "Eigen::VectorXd" }). For each, the generated
    // bindings.h emits
    //   template <typename T>
    //   struct rosetta::is_sequence<GEO::vector<T>> : std::true_type {};
    // (and, for the concrete form, a rosetta::sequence_cpp_name giving the
    // adapter the spelling verbatim) so the container marshals like
    // std::vector<T> in the opted-in expanded backends — see rosetta/sequence.h
    // for the container requirements.
    std::vector<ContainerEntry> sequences;

    // Optional foreign 2-D matrices ("matrices"): same two entry forms as
    // `sequences`, one dimension up. Emitted as rosetta::is_matrix (plus the
    // stated spelling for a concrete type) so the container marshals as an
    // array of row arrays — see rosetta/matrix.h for the requirements.
    std::vector<ContainerEntry> matrices;

    // Optional default for every target's "out_dir" (see TargetEntry::out_dir),
    // absolute. A target's own entry overrides it.
    std::string out_dir;

    // NOTE: `wheel` / `wheel_dir` are per-TARGET — see TargetEntry.
    std::vector<std::string>   plugins;   // extra .cpp sources (absolute) for the driver
    std::vector<std::string>   user_sources; // user .cpp/.c sources (absolute) compiled into the bindings
    std::vector<std::string>   compile_definitions; // "NAME"/"NAME=VALUE" defs for driver + bindings
    std::string                cpp26_root; // optional C++26/P2996 toolchain root (verbatim)
    std::string                cpp26_cxx;  // optional C++ compiler (name or path)
    std::string                cpp26_cc;   // optional C compiler (name or path)
    std::string                cpp26_lib;  // optional fork stdlib dir (libc++/libc++abi)
    std::string                qt_dir;     // optional Qt 6 prefix (qt/qml backends)
    // Optional external libraries the bindings link against ("user_lib"): one
    // object, or an array of them when the bound library itself depends on
    // other pre-built libraries. Order is the link order.
    std::vector<UserLibEntry>  user_libs;
    std::string                build_type;    // optional default CMAKE_BUILD_TYPE for every binding
    std::string                optimization;  // optional explicit -O flag overriding the build type's
    std::string                cxx_standard;  // optional per-source -std for the user_sources ("" ⇒ target's own)
    std::string                version;       // optional distribution version for packaging ("" ⇒ backend default 0.1.0)

    // Optional foreign libraries whose types the target's binding framework can
    // marshal on its own ("interop": ["eigen"]). Emitted as a
    // rosetta::interop_enabled<> specialization into bindings.h; the walk then
    // marks every type the library owns (by enclosing namespace) so a backend
    // with a caster binds it natively and one without skips the member instead
    // of emitting a call that throws. See include/rosetta/interop.h.
    std::vector<std::string>   interop;

    // CMake target / binary basename.
    std::string target() const { return generator_name; }
};

// Parse and validate manifest.json (// and /* */ comments tolerated).
// Relative paths resolve from the manifest's own directory; `user_sources`
// entries may be shell globs, expanded here. Throws std::runtime_error on
// anything malformed.
Manifest load(const fs::path &manifest_path);
