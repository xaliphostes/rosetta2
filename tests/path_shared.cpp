// Copyright (c) fmaerten@gmail.com
// License: MIT

// Google Test suite for the two gaps that kept a factory API off the
// caster-less backends: **std::filesystem::path** and **std::shared_ptr<T>**.
//
// Both are about the same shape of C++ — `std::shared_ptr<Doc> load(const
// std::filesystem::path&)` — and both used to fail in a way that looked like
// nothing: a path was an unregistered class, so every member naming one was
// skipped; a shared_ptr return reached node as an ordinary "object"
// whose `object` is the literal identifier "shared_ptr", which is registered
// nowhere.
//
// What is pinned here:
//
//   * a path is described as a STRING with the is_path flag, and rides the same
//     copy adapter as the foreign containers — boundary std::string, `path{s}`
//     in, `.string()` out;
//   * embind registers `.smart_ptr<std::shared_ptr<T>>` for exactly the classes
//     the module hands out that way, and for no others;
//   * node binds a shared_ptr RETURN (the JS object adopts the pointer)
//     but never a shared_ptr PARAMETER, and never for a trampolined pointee;
//   * sol2 takes the shared_ptr natively once the pointee is bound;
//   * typescript declares the pointee, not "shared_ptr", and `string` for a path.
//
// Verifies the IR and the generated sources (render), not a live build.
//
// Requires: -freflection -freflection-latest -fannotation-attributes

#include <gtest/gtest.h>
#include <rosetta/generate.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

// ---- fixtures ---------------------------------------------------------------

namespace psx {

    // The pointee: plain, no virtuals — what a factory hands out.
    struct Doc {
        int n = 0;
        int size() const { return n; }
    };

    // A pointee WITH virtuals: node cannot adopt a shared_ptr for it (its
    // wrapper holds a trampoline subclass), so that return must stay skipped.
    struct Node {
        virtual ~Node()          = default;
        virtual int  weight() const { return 1; }
        int          id = 0;
    };

    struct Loader {
        // path in, shared_ptr out — the CSGCompiler::compile_file shape.
        std::shared_ptr<Doc>  load_file(const std::filesystem::path &p) {
            auto d = std::make_shared<Doc>();
            d->n   = int(p.string().size());
            return d;
        }
        std::shared_ptr<Doc>  load_string(const std::string &s) {
            auto d = std::make_shared<Doc>();
            d->n   = int(s.size());
            return d;
        }
        std::shared_ptr<Node> load_node() { return std::make_shared<Node>(); }
        // A shared_ptr going the other way: input-only ownership is what node
        // refuses to invent.
        int                   count(const std::shared_ptr<Doc> &d) { return d ? d->n : 0; }
        // path out.
        std::filesystem::path where(const std::filesystem::path &p) { return p; }
    };

} // namespace psx

template <> struct rosetta::binding_info<psx::Doc> {
    static constexpr const char *header = "psx.h";
};
template <> struct rosetta::binding_info<psx::Node> {
    static constexpr const char *header = "psx.h";
};
template <> struct rosetta::binding_info<psx::Loader> {
    static constexpr const char *header = "psx.h";
};

namespace {

    rosetta::GenContext full_context() {
        return rosetta::gen_detail::make_context<psx::Doc, psx::Node, psx::Loader>("psxtest");
    }

    std::string render(const char *lang, const rosetta::GenContext &c) {
        return rosetta::backend_registry().at(lang)->render(c);
    }

    bool has(const std::string &hay, const std::string &needle) {
        return hay.find(needle) != std::string::npos;
    }

} // namespace

// ---- the IR: path -----------------------------------------------------------

TEST(PathShared, PathIsAStringWithTheFlag) {
    const rosetta::GenType t = rosetta::gen_detail::type_descriptor<std::filesystem::path>();
    EXPECT_EQ(t.kind, "string"); // every backend's string gate passes it
    EXPECT_TRUE(t.is_path);
    EXPECT_TRUE(t.element.empty()); // nothing inside a path to describe
    EXPECT_FALSE(t.is_sequence);
    EXPECT_FALSE(t.is_matrix);
}

TEST(PathShared, PathRidesTheCopyAdapter) {
    const rosetta::GenType t = rosetta::gen_detail::type_descriptor<std::filesystem::path>();
    ASSERT_TRUE(rosetta::gen_detail::is_adapted(t));
    EXPECT_TRUE(rosetta::gen_detail::adapt_ok(t));
    EXPECT_EQ(rosetta::gen_detail::adapt_boundary_cpp(t), "std::string");
    EXPECT_EQ(rosetta::gen_detail::adapt_local(t, 0), "pth0");
    EXPECT_EQ(rosetta::gen_detail::adapt_decl_stmts(t, "arg0", "pth0", "    "),
              "    std::filesystem::path pth0(arg0);\n");
    // .string(), not .native(): native() is a wstring on Windows, which no
    // string boundary accepts.
    EXPECT_EQ(rosetta::gen_detail::adapt_from_expr(t, "r"), "(r).string()");
}

// A std::string must NOT pick up the path treatment — it is already the
// boundary type and needs no adapter.
TEST(PathShared, PlainStringIsUntouched) {
    const rosetta::GenType t = rosetta::gen_detail::type_descriptor<std::string>();
    EXPECT_EQ(t.kind, "string");
    EXPECT_FALSE(t.is_path);
    EXPECT_FALSE(rosetta::gen_detail::is_adapted(t));
}

// ---- the IR: shared_ptr -----------------------------------------------------

TEST(PathShared, SharedPointeeResolvesToTheClass) {
    const rosetta::GenType t = rosetta::gen_detail::type_descriptor<std::shared_ptr<psx::Doc>>();
    ASSERT_TRUE(t.is_shared_ptr);
    EXPECT_EQ(t.kind, "object"); // unchanged: a shared_ptr is still the class it is
    EXPECT_EQ(rosetta::gen_detail::shared_pointee(t).object, "Doc");
    // A non-shared_ptr type is its own pointee — callers can ask unconditionally.
    const rosetta::GenType d = rosetta::gen_detail::type_descriptor<psx::Doc>();
    EXPECT_EQ(&rosetta::gen_detail::shared_pointee(d), &d);
}

TEST(PathShared, ModuleWidePointeeCollection) {
    const auto need = rosetta::gen_detail::shared_pointees(full_context());
    EXPECT_NE(std::find(need.begin(), need.end(), "psx::Doc"), need.end());
    EXPECT_NE(std::find(need.begin(), need.end(), "psx::Node"), need.end());
    // Loader itself never travels inside a shared_ptr.
    EXPECT_EQ(std::find(need.begin(), need.end(), "psx::Loader"), need.end());
}

// ---- the generated sources: path -------------------------------------------

// The python family does NOT stringify: pybind11/nanobind have their own path
// caster, so the member binds natively and Python may pass a str or any
// os.PathLike. Stringifying would have been a regression in what callers accept.
TEST(PathShared, PythonFamilyBindsThePathNatively) {
    for (const char *lang : {"python", "nanobind"}) {
        const std::string s = render(lang, full_context());
        EXPECT_TRUE(has(s, "\"load_file\", &psx::Loader::load_file")) << lang;
        EXPECT_FALSE(has(s, "pth0")) << lang; // no adapter, no boundary string
    }
    EXPECT_TRUE(has(render("python", full_context()),
                    "#include <pybind11/stl/filesystem.h>"));
    EXPECT_TRUE(has(render("nanobind", full_context()),
                    "#include <nanobind/stl/filesystem.h>"));
}

TEST(PathShared, NodeExpandedConvertsAtTheBoundary) {
    const std::string s = render("node", full_context());
    EXPECT_TRUE(has(s, "std::filesystem::path pth0(arg0);"));
    EXPECT_TRUE(has(s, "#include <filesystem>"));
}

TEST(PathShared, WasmExpandedConvertsAtTheBoundary) {
    const std::string s = render("wasm", full_context());
    EXPECT_TRUE(has(s, "std::filesystem::path pth0(arg0);"));
}

TEST(PathShared, LuaExpandedConvertsAtTheBoundary) {
    const std::string s = render("lua", full_context());
    EXPECT_TRUE(has(s, "std::filesystem::path pth0(arg0);"));
}

// ---- the generated sources: shared_ptr --------------------------------------

TEST(PathShared, EmbindRegistersSmartPtrForPointeesOnly) {
    const std::string s = render("wasm", full_context());
    EXPECT_TRUE(has(s, ".smart_ptr<std::shared_ptr<psx::Doc>>(\"Doc_sp\")"));
    EXPECT_TRUE(has(s, ".smart_ptr<std::shared_ptr<psx::Node>>(\"Node_sp\")"));
    // Loader is never handed out inside a shared_ptr: no registration.
    EXPECT_FALSE(has(s, "std::shared_ptr<psx::Loader>"));
    EXPECT_TRUE(has(s, "load_string")); // and the factory binds
}

TEST(PathShared, NodeAdoptsASharedPtrReturn) {
    const std::string s = render("node", full_context());
    EXPECT_TRUE(has(s, "load_string")); // Doc has no virtuals: adoptable
    EXPECT_FALSE(has(s, "load_node"));  // Node has virtuals: its wrapper is a trampoline
    EXPECT_FALSE(has(s, "\"count\""));  // a shared_ptr PARAMETER stays out
}

TEST(PathShared, LuaTakesTheSharedPtrNatively) {
    const std::string s = render("lua", full_context());
    EXPECT_TRUE(has(s, "load_string"));
    EXPECT_TRUE(has(s, "load_node")); // sol2 has no trampoline restriction
}

TEST(PathShared, PythonKeepsItsHolder) {
    const std::string s = render("python", full_context());
    EXPECT_TRUE(has(s, "py::class_<psx::Doc, std::shared_ptr<psx::Doc>>"));
}

// ---- typescript -------------------------------------------------------------

// typescript writes its .d.ts in emit(), so the declarations have to be read
// off the generated file rather than off render().
TEST(PathShared, TypescriptDeclaresPointeeAndString) {
    namespace fs       = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rosetta_path_shared_ts";
    fs::remove_all(dir);
    auto c    = full_context();
    c.out_dir = dir;
    rosetta::backend_registry().at("typescript")->emit(c);

    std::ifstream     in(dir / "typescript" / "psxtest.d.ts");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    fs::remove_all(dir);

    ASSERT_FALSE(s.empty());
    // The parameter is named `p` in the header, and the walk reads that name —
    // the ".d.ts" is what an editor shows on completion, so it says `p`.
    EXPECT_TRUE(has(s, "load_file(p: string): Doc;")); // not "shared_ptr", not "any"
    EXPECT_TRUE(has(s, "where(p: string): string;"));
}
