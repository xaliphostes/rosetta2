// Copyright (c) fmaerten@gmail.com
// License: MIT

// Google Test suite for PARAMETER NAMES and DEFAULT ARGUMENTS in the IR.
//
// Until now every GenParam was called "argN": the walk decomposed a signature
// into types and threw the declaration away, so a generated SDK read
// `f(arg0, arg1)` and no backend could offer a keyword argument, an optional
// parameter or a documented one. rosetta::gen_detail::params_of now reads each
// parameter's own identifier (std::meta::identifier_of) and whether it carries
// a default argument (std::meta::has_default_argument).
//
// The suite pins four things:
//   - names come from the declaration, in order, for instance / static /
//     free functions and constructors;
//   - a parameter declared WITHOUT a name keeps its positional fallback, and
//     the fallback is keyed on its own index (so a half-named signature does
//     not shift);
//   - has_default marks exactly the parameters that have one;
//   - names do not leak between functions. That last one is not paranoia: the
//     obvious implementation — a helper templated on the parameter reflection —
//     has its instantiations collapse under clang-p2996, so every function in
//     the TU reports the names of whichever was instantiated last. The
//     multi-function expectations below are what catch a regression to it, so
//     keep several differently-shaped functions in one test binary.

#include <gtest/gtest.h>
#include <rosetta/generate.h>
#include <string>
#include <vector>

namespace {

    enum class Mode { Fast, Slow };

    struct Shape {
        double area(double radius, int segments = 32, bool closed = true) const;
        // A deliberately unnamed leading parameter, as interface headers write
        // when an argument is accepted and ignored.
        double        blend(double, int weight) const;
        static double unit(double scale);
        void          configure(Mode mode = Mode::Fast, const std::string &tag = "default");
        double        none() const;

        Shape() = default;
        Shape(double width, double height);
    };

    double distance(const Shape &from, const Shape &to, bool squared = false);

    // Names / defaults of one function's parameters, in declaration order.
    template <std::meta::info Fn> std::vector<rosetta::GenParam> params() {
        return rosetta::gen_detail::params_of<Fn>();
    }

    std::vector<std::string> names_of(const std::vector<rosetta::GenParam> &ps) {
        std::vector<std::string> out;
        for (const auto &p : ps) {
            out.push_back(p.name);
        }
        return out;
    }

    std::vector<bool> defaults_of(const std::vector<rosetta::GenParam> &ps) {
        std::vector<bool> out;
        for (const auto &p : ps) {
            out.push_back(p.has_default);
        }
        return out;
    }

    // ---- names come from the declaration -----------------------------------

    TEST(ParamNames, InstanceMethodReadsDeclaredNames) {
        EXPECT_EQ(names_of(params<^^Shape::area>()),
                  (std::vector<std::string>{"radius", "segments", "closed"}));
    }

    TEST(ParamNames, StaticMethodReadsDeclaredNames) {
        EXPECT_EQ(names_of(params<^^Shape::unit>()), (std::vector<std::string>{"scale"}));
    }

    TEST(ParamNames, FreeFunctionReadsDeclaredNames) {
        EXPECT_EQ(names_of(params<^^distance>()),
                  (std::vector<std::string>{"from", "to", "squared"}));
    }

    TEST(ParamNames, NoParametersYieldsEmptyList) {
        EXPECT_TRUE(params<^^Shape::none>().empty());
    }

    // ---- the positional fallback survives, keyed on the real index ---------

    TEST(ParamNames, UnnamedParameterKeepsPositionalFallback) {
        // arg0 (not "arg1", and not the next name shifted down): the fallback
        // is the parameter's OWN index, so the named tail stays put.
        EXPECT_EQ(names_of(params<^^Shape::blend>()), (std::vector<std::string>{"arg0", "weight"}));
    }

    // ---- default arguments -------------------------------------------------

    TEST(ParamNames, DefaultArgumentsAreFlagged) {
        EXPECT_EQ(defaults_of(params<^^Shape::area>()), (std::vector<bool>{false, true, true}));
    }

    TEST(ParamNames, NonDefaultedParametersAreNotFlagged) {
        EXPECT_EQ(defaults_of(params<^^Shape::blend>()), (std::vector<bool>{false, false}));
        EXPECT_EQ(defaults_of(params<^^Shape::unit>()), (std::vector<bool>{false}));
    }

    TEST(ParamNames, DefaultsOfNonScalarTypesAreFlaggedToo) {
        // An enum and a std::string default: reflection reports the FACT for
        // any type. (The default's spelling is a separate, textual concern —
        // see the doc-comment harvester.)
        EXPECT_EQ(names_of(params<^^Shape::configure>()),
                  (std::vector<std::string>{"mode", "tag"}));
        EXPECT_EQ(defaults_of(params<^^Shape::configure>()), (std::vector<bool>{true, true}));
    }

    // ---- no leakage between functions --------------------------------------

    TEST(ParamNames, NamesDoNotLeakBetweenFunctions) {
        // The regression guard described at the top of this file: read several
        // differently-shaped signatures in ONE test and require each to keep
        // its own names. Under the collapsing implementation these all report
        // the last-instantiated function's parameters instead.
        const auto area = names_of(params<^^Shape::area>());
        const auto unit = names_of(params<^^Shape::unit>());
        const auto dist = names_of(params<^^distance>());
        const auto conf = names_of(params<^^Shape::configure>());

        EXPECT_EQ(area, (std::vector<std::string>{"radius", "segments", "closed"}));
        EXPECT_EQ(unit, (std::vector<std::string>{"scale"}));
        EXPECT_EQ(dist, (std::vector<std::string>{"from", "to", "squared"}));
        EXPECT_EQ(conf, (std::vector<std::string>{"mode", "tag"}));
    }

    TEST(ParamNames, DefaultsDoNotLeakBetweenFunctions) {
        EXPECT_EQ(defaults_of(params<^^Shape::area>()), (std::vector<bool>{false, true, true}));
        EXPECT_EQ(defaults_of(params<^^distance>()), (std::vector<bool>{false, false, true}));
        EXPECT_EQ(defaults_of(params<^^Shape::unit>()), (std::vector<bool>{false}));
    }

} // namespace
