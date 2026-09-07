// Copyright (c) fmaerten@gmail.com
// License: MIT

// Google Test suite for std::shared_ptr support and the two neighbouring
// python/nanobind emission fixes.
//
// A std::shared_ptr<T> is described in the IR as the class it is (kind
// "object", so no backend's marshalling gate changes) plus the new
// GenType::is_shared_ptr flag and the pointee in `element`. python
// consumes it: every bound class that ever crosses the boundary inside a
// shared_ptr is registered as py::class_<T, std::shared_ptr<T>> — without the
// holder pybind11 compiles happily and throws at CALL time. pybind requires
// ONE holder per inheritance chain, so the set is propagated up and down the
// bound base links.
//
// Also covered here, same failure family (compiles fine, breaks later):
//   - nanobind's one-caster-per-header includes (a signature naming any std
//     type beyond string/vector/function threw at call time),
//   - PYBIND11_OVERRIDE with a comma-bearing return type (the PREPROCESSOR
//     split it across two macro arguments),
//   - nb::init<> emitted for an abstract class (ill-formed `new T{}`).
//
// Verifies the generated sources (render), not a live build — mirroring
// sequence.cpp / member_object.cpp.
//
// Requires: -freflection -freflection-latest -fannotation-attributes

#include <gtest/gtest.h>
#include <memory>
#include <rosetta/generate.h>
#include <string>
#include <vector>

namespace spns {
    // Abstract interface: handed across the boundary only inside a shared_ptr,
    // and never constructible from script.
    struct Executor {
        virtual ~Executor()              = default;
        virtual int run(int n)           = 0;
        // Comma-bearing return type: the exact spelling is
        // vector<double, allocator<double>>, which the override macro would
        // split across two arguments.
        virtual std::vector<double> profile() const { return {}; }
    };

    struct LocalExecutor : Executor {
        int run(int n) override { return n; }
    };

    struct RemoteExecutor : Executor {
        int run(int n) override { return -n; }
    };

    // shared_ptr in a RETURN type.
    struct Engine {
        int                       jobs = 1;
        std::shared_ptr<Executor> local() const { return std::make_shared<LocalExecutor>(); }
    };

    // shared_ptr in a PARAMETER type.
    struct Scheduler {
        void submit(std::shared_ptr<Executor> e) { (void)e; }
    };

    // shared_ptr through a free function.
    inline std::shared_ptr<Executor> make_remote() { return std::make_shared<RemoteExecutor>(); }
} // namespace spns

template <> struct rosetta::binding_info<spns::Executor> {
    static constexpr const char *header = "sp.h";
};
template <> struct rosetta::binding_info<spns::LocalExecutor> {
    static constexpr const char *header = "sp.h";
};
template <> struct rosetta::binding_info<spns::RemoteExecutor> {
    static constexpr const char *header = "sp.h";
};
template <> struct rosetta::binding_info<spns::Engine> {
    static constexpr const char *header = "sp.h";
};
template <> struct rosetta::binding_info<spns::Scheduler> {
    static constexpr const char *header = "sp.h";
};

// The whole family: the interface, its two implementations, and the class
// whose factory hands one out.
static rosetta::GenContext full_context() {
    return rosetta::gen_detail::make_context<spns::Executor, spns::LocalExecutor,
                                             spns::RemoteExecutor, spns::Engine>("sptest");
}

static std::string render(const char *lang, const rosetta::GenContext &c) {
    return rosetta::backend_registry().at(lang)->render(c);
}

static std::size_t count_of(const std::string &hay, const std::string &needle) {
    std::size_t n = 0;
    for (std::size_t i = hay.find(needle); i != std::string::npos;
         i             = hay.find(needle, i + needle.size())) {
        ++n;
    }
    return n;
}

// ---- IR ----------------------------------------------------------------------

TEST(SharedPtr, WalkFlagsItAndRecordsThePointee) {
    const auto c = full_context();
    const rosetta::GenMethod *local = nullptr;
    for (const auto &k : c.classes) {
        for (const auto &m : k.methods) {
            if (m.name == "local") {
                local = &m;
            }
        }
    }
    ASSERT_NE(local, nullptr);
    EXPECT_TRUE(local->ret.is_shared_ptr);
    // NOT a new kind — a shared_ptr is still described as an object, so the
    // backends that don't declare holders keep binding it exactly as before.
    EXPECT_EQ(local->ret.kind, "object");
    ASSERT_EQ(local->ret.element.size(), 1u);
    EXPECT_EQ(local->ret.element[0].object_qualified, "spns::Executor");
}

TEST(SharedPtr, PlainTypesAreNotFlagged) {
    const auto c = full_context();
    for (const auto &k : c.classes) {
        for (const auto &m : k.methods) {
            if (m.name == "run" || m.name == "profile") {
                EXPECT_FALSE(m.ret.is_shared_ptr) << m.name;
            }
        }
    }
}

// ---- python: holders ------------------------------------------------

TEST(SharedPtr, PythonDeclaresTheHolderOnThePointee) {
    const std::string s = render("python", full_context());
    EXPECT_NE(s.find("py::class_<spns::Executor, std::shared_ptr<spns::Executor>"),
              std::string::npos);
    // The holder spelling needs <memory>.
    EXPECT_NE(s.find("#include <memory>"), std::string::npos);
}

TEST(SharedPtr, PythonPropagatesTheHolderAcrossTheInheritanceChain) {
    // Only Executor crosses the boundary; pybind still requires one holder per
    // chain, so both implementations must carry it too.
    const std::string s = render("python", full_context());
    EXPECT_NE(s.find("py::class_<spns::LocalExecutor, std::shared_ptr<spns::LocalExecutor>"),
              std::string::npos);
    EXPECT_NE(s.find("py::class_<spns::RemoteExecutor, std::shared_ptr<spns::RemoteExecutor>"),
              std::string::npos);
}

TEST(SharedPtr, PythonLeavesUnrelatedClassesOnTheDefaultHolder) {
    const std::string s = render("python", full_context());
    // Engine hands one out but never travels inside one itself.
    EXPECT_NE(s.find("py::class_<spns::Engine> c(m, \"Engine\");"), std::string::npos);
    EXPECT_EQ(s.find("std::shared_ptr<spns::Engine>"), std::string::npos);
}

TEST(SharedPtr, PythonDeclaresNoHolderWhenNothingCrosses) {
    // Same three classes, minus the factory: no shared_ptr anywhere in the
    // surface, so the output is the pre-change one.
    const auto c = rosetta::gen_detail::make_context<spns::Executor, spns::LocalExecutor,
                                                     spns::RemoteExecutor>("sptest");
    const std::string s = render("python", c);
    EXPECT_EQ(s.find("std::shared_ptr<spns::"), std::string::npos);
}

TEST(SharedPtr, PythonHolderFromAParameter) {
    const auto        c = rosetta::gen_detail::make_context<spns::Executor, spns::Scheduler>("sptest");
    const std::string s = render("python", c);
    EXPECT_NE(s.find("py::class_<spns::Executor, std::shared_ptr<spns::Executor>"),
              std::string::npos);
    EXPECT_EQ(s.find("std::shared_ptr<spns::Scheduler>"), std::string::npos);
}

TEST(SharedPtr, PythonHolderFromAFreeFunction) {
    auto c = rosetta::gen_detail::make_context<spns::Executor>("sptest");
    c.functions.push_back(
        rosetta::make_function<^^spns::make_remote>("spns::make_remote", "sp.h", ""));
    const std::string s = render("python", c);
    EXPECT_NE(s.find("py::class_<spns::Executor, std::shared_ptr<spns::Executor>"),
              std::string::npos);
}

// ---- python (thin): the override-macro comma --------------------------------

TEST(SharedPtr, CommaBearingReturnTypeHidesBehindALocalAlias) {
    const auto c = rosetta::gen_detail::make_context<spns::Executor>("sptest");
    const std::string s = render("python", c);
    EXPECT_NE(s.find("using rosetta_ret_t = vector<double, allocator<double>>;"),
              std::string::npos);
    EXPECT_NE(s.find("PYBIND11_OVERRIDE(rosetta_ret_t, spns::Executor, profile, );"),
              std::string::npos);
    // The unaliased spelling would have been split by the preprocessor.
    EXPECT_EQ(s.find("PYBIND11_OVERRIDE(vector<double, allocator<double>>"), std::string::npos);
}

TEST(SharedPtr, CommaFreeReturnTypeIsUnchanged) {
    const auto c = rosetta::gen_detail::make_context<spns::Executor>("sptest");
    const std::string s = render("python", c);
    // run() returns int — no alias, byte-identical to the pre-change output.
    EXPECT_NE(s.find("PYBIND11_OVERRIDE_PURE(int, spns::Executor, run, p0);"), std::string::npos);
    // Exactly one alias in the whole file: profile()'s.
    EXPECT_EQ(count_of(s, "using rosetta_ret_t ="), 1u);
}

TEST(SharedPtr, ExpandedPythonSharesTheSameTrampolineFix) {
    const auto c = rosetta::gen_detail::make_context<spns::Executor>("sptest");
    const std::string s = render("python", c);
    EXPECT_NE(s.find("using rosetta_ret_t = vector<double, allocator<double>>;"),
              std::string::npos);
    EXPECT_NE(s.find("PYBIND11_OVERRIDE(rosetta_ret_t, spns::Executor, profile, );"),
              std::string::npos);
}

// ---- nanobind -------------------------------------------------------

TEST(SharedPtr, NanobindEmitsNoConstructorForAnAbstractClass) {
    // nb::init<> instantiates the Alias type (== T, no trampoline here), so
    // `new Executor{}` on the pure-virtual interface would be ill-formed. The
    // walk still records an implicit default ctor, hence the guard on the loop.
    const auto c = rosetta::gen_detail::make_context<spns::Executor>("sptest");
    const std::string s = render("nanobind", c);
    EXPECT_NE(s.find("nb::class_<spns::Executor>(m, \"Executor\")"), std::string::npos);
    EXPECT_EQ(s.find("nb::init"), std::string::npos);
    // `run(int n)` declares its parameter, so the binding offers it as a
    // keyword argument — nanobind and pybind spell that the same way.
    EXPECT_NE(s.find(".def(\"run\", &spns::Executor::run, nb::arg(\"n\"))"), std::string::npos);
}

TEST(SharedPtr, NanobindStillConstructsAConcreteClass) {
    const auto c = rosetta::gen_detail::make_context<spns::LocalExecutor>("sptest");
    const std::string s = render("nanobind", c);
    EXPECT_NE(s.find(".def(nb::init<>())"), std::string::npos);
}

TEST(SharedPtr, NanobindPullsInTheStlCasterHeaders) {
    // nanobind ships one caster per header (pybind11 has them all in stl.h);
    // a missing one compiles and then throws at call time.
    const std::string s = render("nanobind", full_context());
    for (const char *h : {"shared_ptr", "unique_ptr", "variant", "optional", "pair", "tuple",
                          "array", "map", "unordered_map", "set", "unordered_set", "string",
                          "vector", "function"}) {
        EXPECT_NE(s.find(std::string("#include <nanobind/stl/") + h + ".h>"), std::string::npos)
            << h;
    }
}
