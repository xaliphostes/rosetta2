// Copyright (c) fmaerten@gmail.com
// License: MIT

// Google Test suite for OUT-PARAMETERS — the manifest's "out_params".
//
// `bool get_doubles(const std::string &name, vector<double> &out, index_t &dim)`
// is how a C++ API returns three things, and no host language has that shape:
// every backend skipped such a member, because the argument its runtime converts
// is a temporary and cannot bind to a non-const reference. The adapter that
// already exists for foreign containers is where it belongs — declare a LOCAL,
// pass that, and hand its value back with the return value.
//
// The design decision this suite pins hardest is that an out-parameter is
// **declared, never inferred**. `void assign_points(vector<double> &pts,
// index_t dim, bool steal)` takes its vector as an INPUT it may steal from;
// `get_doubles` fills its own as an OUTPUT. The two are indistinguishable in
// C++, and guessing wrong silently drops either an argument or a result — so
// the manifest says which, and an unmarked mutable reference keeps its old
// behaviour exactly.
//
// Also pinned: the per-language shape (a tuple in Python, multiple returns in
// Lua, an array in JS — a std::tuple everywhere except embind, which has no
// tuple and builds an emscripten::val array), and that the parameter leaves the
// exposed signature.
//
// Requires: -freflection -freflection-latest -fannotation-attributes

#include <gtest/gtest.h>
#include <rosetta/generate.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ---- fixtures ---------------------------------------------------------------

namespace opx {

    // A foreign container, registered as a sequence — the shape geogram's
    // GEO::vector has, and the interesting out-parameter type.
    template <typename T> struct fvec : std::vector<T> {
        using std::vector<T>::vector;
    };

    struct Attrs {
        // The out-parameter case: a bool return plus two written-through refs.
        bool get_doubles(const std::string &name, fvec<double> &out, unsigned &dim) const {
            (void)name;
            out.assign({1.0, 2.0, 3.0, 4.0});
            dim = 2;
            return true;
        }
        // The INPUT case, same C++ shape: the manifest does not mark it, so it
        // must keep binding as an ordinary (input) parameter.
        void assign(fvec<double> &pts, unsigned dim) {
            (void)pts;
            (void)dim;
        }
        // A scalar-only out-parameter.
        void measure(unsigned &count) const { count = 7; }
    };

} // namespace opx

template <typename T> struct rosetta::is_sequence<opx::fvec<T>> : std::true_type {};

template <> struct rosetta::binding_info<opx::Attrs> {
    static constexpr const char *header = "opx.h";
};

namespace {

    // The manifest's "out_params" is applied by generate(); make_context() does
    // not run it, so the marks are set the way generate() would.
    rosetta::GenContext marked_context() {
        auto c = rosetta::gen_detail::make_context<opx::Attrs>("optest");
        for (auto &k : c.classes) {
            for (auto &m : k.methods) {
                if (m.name == "get_doubles") {
                    m.params[1].is_out = true;
                    m.params[2].is_out = true;
                } else if (m.name == "measure") {
                    m.params[0].is_out = true;
                }
            }
        }
        return c;
    }

    std::string render(const char *lang, const rosetta::GenContext &c) {
        return rosetta::backend_registry().at(lang)->render(c);
    }

    bool has(const std::string &hay, const std::string &needle) {
        return hay.find(needle) != std::string::npos;
    }

} // namespace

// ---- the mark ---------------------------------------------------------------

TEST(OutParams, NeverInferred) {
    // Same C++ shape, no mark: not an out-parameter.
    const auto c = rosetta::gen_detail::make_context<opx::Attrs>("optest");
    for (const auto &m : c.classes.front().methods) {
        for (const auto &p : m.params) {
            EXPECT_FALSE(rosetta::gen_detail::is_out_param(p)) << m.name;
        }
    }
    // And with the mark, it is.
    const auto marked = marked_context();
    for (const auto &m : marked.classes.front().methods) {
        if (m.name == "get_doubles") {
            EXPECT_FALSE(rosetta::gen_detail::is_out_param(m.params[0])); // const string&
            EXPECT_TRUE(rosetta::gen_detail::is_out_param(m.params[1]));  // fvec<double>&
            EXPECT_TRUE(rosetta::gen_detail::is_out_param(m.params[2]));  // unsigned&
        }
    }
}

// A mark on a const reference means nothing: the callee cannot write to it.
TEST(OutParams, ConstReferenceIsNeverAnOutput) {
    rosetta::GenParam p;
    p.is_out         = true;
    p.is_ref         = true;
    p.is_mutable_ref = false;
    p.type.kind      = "string";
    EXPECT_FALSE(rosetta::gen_detail::is_out_param(p));
}

// ---- the emitted adapters ---------------------------------------------------

TEST(OutParams, PythonReturnsATuple) {
    const std::string s = render("python", marked_context());
    // The out-parameters are locals, not boundary arguments...
    EXPECT_TRUE(has(s, "opx::fvec<double> out1{};"));
    EXPECT_TRUE(has(s, "unsigned int out2{};"));
    // ...the exposed lambda takes only the real input...
    EXPECT_TRUE(has(s, "c.def(\"get_doubles\", [](opx::Attrs &self, const std::string & arg0)"));
    // ...and the values come back with the return, the sequence flattened.
    EXPECT_TRUE(has(s, "return std::make_tuple(r, std::vector<double>(out1.begin(), "
                       "out1.end()), out2);"));
}

TEST(OutParams, VoidReturnYieldsOnlyTheOutputs) {
    const std::string s = render("python", marked_context());
    EXPECT_TRUE(has(s, "return std::make_tuple(out0);"));
}

TEST(OutParams, AnUnmarkedMutableSequenceStaysAnInput) {
    const std::string s = render("python", marked_context());
    // `assign` keeps the ordinary adapter: a boundary vector in, nothing out.
    EXPECT_TRUE(has(s, "c.def(\"assign\", [](opx::Attrs &self, std::vector<double> arg0"));
    EXPECT_FALSE(has(s, "out0{};\n        opx::fvec<double> "));
}

TEST(OutParams, NodeReturnsATupleForJs) {
    const std::string s = render("node", marked_context());
    EXPECT_TRUE(has(s, "std::tuple<bool, std::vector<double>, unsigned int>"));
    EXPECT_TRUE(has(s, "return std::make_tuple(r, "));
}

TEST(OutParams, LuaReturnsATupleSolUnpacks) {
    const std::string s = render("lua", marked_context());
    EXPECT_TRUE(has(s, "return std::make_tuple(r, "));
}

// embind marshals no tuple, so wasm builds a val array instead — the same JS
// shape node hands out, reached a different way.
TEST(OutParams, WasmBuildsAValArray) {
    const std::string s = render("wasm", marked_context());
    EXPECT_TRUE(has(s, "emscripten::val out = emscripten::val::array();"));
    EXPECT_TRUE(has(s, "out.set(0, rosetta_wx::to_val(r));"));
    EXPECT_TRUE(has(s, "return out;"));
    EXPECT_FALSE(has(s, "std::make_tuple")); // would not compile through embind
}

// ---- the declaration --------------------------------------------------------

TEST(OutParams, TypescriptDeclaresTheTuple) {
    namespace fs       = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rosetta_out_params_ts";
    fs::remove_all(dir);
    auto c    = marked_context();
    c.out_dir = dir;
    rosetta::backend_registry().at("typescript")->emit(c);

    std::ifstream     in(dir / "typescript" / "optest.d.ts");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    fs::remove_all(dir);

    ASSERT_FALSE(s.empty());
    // `name` is the declared parameter; `out` and `dim` are out-parameters and
    // leave the signature, so the reflected names of the two that remain are
    // exactly what the declaration spells.
    EXPECT_TRUE(has(s, "get_doubles(name: string): [boolean, number[], number];"));
    EXPECT_TRUE(has(s, "measure(): [number];"));
}
