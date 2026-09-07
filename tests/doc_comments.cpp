// Copyright (c) fmaerten@gmail.com
// License: MIT

// Google Test suite for the DOC-COMMENT HARVESTER
// (tools/rosetta_gen/doccomments.cpp): the lexical pass that reads the
// documentation an existing library already carries — `///` and `/** */`
// blocks, `@param`, `@return` — out of its headers, plus the default arguments
// reflection cannot report.
//
// The subject is the TOOL, so this is plain C++17 with no reflection and no
// annotations: feed it header text, inspect the DocMap.
//
// What matters most here is what the harvester REFUSES. It is a scanner, not a
// compiler, and everything it records is later spliced into generated C++ or
// attached to a binding — so a wrong reading is worse than no reading. The
// "declines" tests below are therefore as load-bearing as the "reads" ones.

#include <doccomments.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

    const DocEntryInfo *first(const DocMap &m, const std::string &key) {
        DocMap::const_iterator it = m.find(key);
        if (it == m.end() || it->second.empty()) {
            return nullptr;
        }
        return &it->second.front();
    }

    std::vector<std::string> param_names(const DocEntryInfo &e) {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < e.params.size(); ++i) {
            out.push_back(e.params[i].name);
        }
        return out;
    }

    // ---- comments attach to the declaration that follows --------------------

    TEST(DocComments, TripleSlashBlockAttachesToMethod) {
        const DocMap        m = harvest_doc_comments(R"(
struct Shape {
    /// Compute the area of the shape.
    /// Uses the cached radius when one is available.
    double area() const;
};
)");
        const DocEntryInfo *e = first(m, "Shape::area");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->doc, "Compute the area of the shape. Uses the cached radius when one is "
                          "available.");
        EXPECT_TRUE(e->is_function);
    }

    TEST(DocComments, BlockCommentWithBriefAndParams) {
        const DocMap        m = harvest_doc_comments(R"(
struct Shape {
    /**
     * @brief Scale the shape.
     * @param factor how much to scale by
     * @param about  the fixed point
     * @return the new area
     */
    double scale(double factor, const Point &about);
};
)");
        const DocEntryInfo *e = first(m, "Shape::scale");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->doc, "Scale the shape.");
        EXPECT_EQ(e->returns, "the new area");
        ASSERT_EQ(e->params.size(), 2u);
        EXPECT_EQ(e->params[0].name, "factor");
        EXPECT_EQ(e->params[0].doc, "how much to scale by");
        EXPECT_EQ(e->params[1].name, "about");
        EXPECT_EQ(e->params[1].doc, "the fixed point");
    }

    TEST(DocComments, ParamTextContinuesOnFollowingLines) {
        const DocMap        m = harvest_doc_comments(R"(
struct Mesh {
    /**
     * @param tolerance the distance below which two vertices are
     *        considered the same point
     * @param verbose   print progress
     */
    void weld(double tolerance, bool verbose);
};
)");
        const DocEntryInfo *e = first(m, "Mesh::weld");
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->params.size(), 2u);
        EXPECT_EQ(e->params[0].doc,
                  "the distance below which two vertices are considered the same point");
        EXPECT_EQ(e->params[1].doc, "print progress");
    }

    TEST(DocComments, DirectionMarkersAreStripped) {
        const DocMap        m = harvest_doc_comments(R"(
struct Solver {
    /// @param[in]     lhs the left side
    /// @param[in,out] rhs the right side, overwritten with the result
    void solve(const Matrix &lhs, Matrix &rhs);
};
)");
        const DocEntryInfo *e = first(m, "Solver::solve");
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->params.size(), 2u);
        EXPECT_EQ(e->params[0].doc, "the left side");
        EXPECT_EQ(e->params[1].doc, "the right side, overwritten with the result");
    }

    TEST(DocComments, NotesAndWarningsBecomeLabelledParagraphs) {
        const DocMap        m = harvest_doc_comments(R"(
struct Grid {
    /// Refine the grid.
    /// @note This invalidates every existing handle.
    /// @warning Not thread safe.
    void refine();
};
)");
        const DocEntryInfo *e = first(m, "Grid::refine");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->doc, "Refine the grid.\n\nNote: This invalidates every existing handle."
                          "\n\nWarning: Not thread safe.");
    }

    TEST(DocComments, FieldsAndClassesAreHarvested) {
        const DocMap        m   = harvest_doc_comments(R"(
/// A point in the plane.
struct Point {
    /// The horizontal coordinate.
    double x = 0;
    double y = 0;   ///< The vertical coordinate.
};
)");
        const DocEntryInfo *cls = first(m, "Point");
        ASSERT_NE(cls, nullptr);
        EXPECT_EQ(cls->doc, "A point in the plane.");

        const DocEntryInfo *x = first(m, "Point::x");
        ASSERT_NE(x, nullptr);
        EXPECT_EQ(x->doc, "The horizontal coordinate.");
        EXPECT_FALSE(x->is_function);

        const DocEntryInfo *y = first(m, "Point::y");
        ASSERT_NE(y, nullptr);
        EXPECT_EQ(y->doc, "The vertical coordinate.");
    }

    TEST(DocComments, StaleParamNameIsDroppedRatherThanMisplaced) {
        // `@param radius` names a parameter the signature no longer has. Putting
        // its text on `diameter` by position would be actively wrong.
        const DocMap        m = harvest_doc_comments(R"(
struct Circle {
    /// @param radius the radius
    double area(double diameter);
};
)");
        const DocEntryInfo *e = first(m, "Circle::area");
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->params.size(), 1u);
        EXPECT_EQ(e->params[0].name, "diameter");
        EXPECT_TRUE(e->params[0].doc.empty());
    }

    // ---- namespaces, access, keys ------------------------------------------

    TEST(DocComments, KeysAreStoredQualifiedAndUnqualified) {
        const DocMap m = harvest_doc_comments(R"(
namespace geom {
    struct Box {
        /// The volume.
        double volume() const;
    };
}
)");
        ASSERT_NE(first(m, "geom::Box::volume"), nullptr);
        ASSERT_NE(first(m, "Box::volume"), nullptr);
        EXPECT_EQ(first(m, "geom::Box::volume")->doc, "The volume.");
        EXPECT_EQ(first(m, "Box::volume")->doc, "The volume.");
    }

    TEST(DocComments, NestedNamespaceShorthandIsTracked) {
        const DocMap m = harvest_doc_comments(R"(
namespace a::b {
    struct C {
        /// Doc.
        void f();
    };
}
)");
        EXPECT_NE(first(m, "a::b::C::f"), nullptr);
    }

    TEST(DocComments, PrivateMembersAreNotHarvested) {
        const DocMap m = harvest_doc_comments(R"(
class Widget {
    /// Internal scratch buffer.
    int cache = 0;
public:
    /// The visible count.
    int count = 0;
private:
    /// Also internal.
    void reset();
};
)");
        EXPECT_EQ(first(m, "Widget::cache"), nullptr);
        EXPECT_EQ(first(m, "Widget::reset"), nullptr);
        ASSERT_NE(first(m, "Widget::count"), nullptr);
        EXPECT_EQ(first(m, "Widget::count")->doc, "The visible count.");
    }

    TEST(DocComments, QualifiedFieldTypesAreNotMistakenForBitFields) {
        // Regression: the bit-field cut used the first ':' in the declaration,
        // which is the first colon of the '::' in a qualified type. Every field
        // whose type was qualified AND which had no initializer to cut at first
        // was silently dropped — `std::string title;` in a plain header.
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// The title.
    std::string title;
    /// A map.
    std::map<int, geom::Point> table;
    /// Flags.
    unsigned flags : 3;
};
)");
        ASSERT_NE(first(m, "S::title"), nullptr);
        EXPECT_EQ(first(m, "S::title")->doc, "The title.");
        ASSERT_NE(first(m, "S::table"), nullptr);
        EXPECT_EQ(first(m, "S::table")->doc, "A map.");
        // The real bit-field still reads as one: `flags`, not `unsigned`.
        ASSERT_NE(first(m, "S::flags"), nullptr);
        EXPECT_EQ(first(m, "S::flags")->doc, "Flags.");
    }

    TEST(DocComments, StructMembersArePublicByDefault) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    int v = 0;
};
)");
        EXPECT_NE(first(m, "S::v"), nullptr);
    }

    TEST(DocComments, InlineRosettaDocAnnotationWins) {
        // The author annotated the member directly; a harvested comment must not
        // silently outrank what they wrote for rosetta specifically.
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// A comment.
    [[= rosetta::doc{"the annotation"}]] int v = 0;
};
)");
        EXPECT_EQ(first(m, "S::v"), nullptr);
    }

    // ---- parameter names ----------------------------------------------------

    TEST(DocComments, ParameterNamesAreReadFromTheDeclaration) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    void f(const std::string &path, std::vector<int> counts, double *out, int n[3]);
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(param_names(*e), (std::vector<std::string>{"path", "counts", "out", "n"}));
    }

    TEST(DocComments, UnnamedParametersReadAsEmpty) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    void f(double, const Mode, std::vector<Foo>, int n);
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(param_names(*e), (std::vector<std::string>{"", "", "", "n"}));
    }

    TEST(DocComments, TemplateArgumentCommasDoNotSplitParameters) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    void f(std::map<int, double> table, std::pair<int, int> range);
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(param_names(*e), (std::vector<std::string>{"table", "range"}));
    }

    TEST(DocComments, VoidParameterListIsEmpty) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    int f(void);
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        EXPECT_TRUE(e->params.empty());
    }

    // ---- default arguments --------------------------------------------------

    TEST(DocComments, SafeDefaultsAreCaptured) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    void f(int segments = 32, double eps = 1e-6, bool closed = true,
           const std::string &tag = "auto", Mode mode = Mode::Fast, int flags = {});
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->params.size(), 6u);
        EXPECT_EQ(e->params[0].default_text, "32");
        EXPECT_EQ(e->params[1].default_text, "1e-6");
        EXPECT_EQ(e->params[2].default_text, "true");
        EXPECT_EQ(e->params[3].default_text, "\"auto\"");
        EXPECT_EQ(e->params[4].default_text, "Mode::Fast");
        EXPECT_EQ(e->params[5].default_text, "{}");
    }

    TEST(DocComments, UnsafeDefaultsAreDeclined) {
        // Each of these would either not compile where the binding spells it, or
        // not mean there what it means here. None may be captured.
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    void f(Point p = Point(0, 0), int n = compute(), int k = a + b,
           Mode m = Fast, Vec v = {1, 2, 3}, int z = sizeof(int));
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->params.size(), 6u);
        for (std::size_t i = 0; i < e->params.size(); ++i) {
            EXPECT_EQ(e->params[i].default_text, "") << "parameter " << i;
        }
    }

    TEST(DocComments, NegativeAndSuffixedNumbersAreCaptured) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    /// Doc.
    void f(int a = -1, unsigned b = 0u, float c = 0.5f, long d = 0x10L);
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(e->params.size(), 4u);
        EXPECT_EQ(e->params[0].default_text, "-1");
        EXPECT_EQ(e->params[1].default_text, "0u");
        EXPECT_EQ(e->params[2].default_text, "0.5f");
        EXPECT_EQ(e->params[3].default_text, "0x10L");
    }

    // ---- what the scanner must step over ------------------------------------

    TEST(DocComments, OperatorsAreSkipped) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Compare.
    bool operator==(const S &other) const;
};
)");
        EXPECT_TRUE(m.find("S::operator") == m.end());
        EXPECT_EQ(first(m, "S::operator=="), nullptr);
    }

    TEST(DocComments, ConstructorsAndDestructorsAreSkipped) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Build one.
    S(int a, int b);
    /// Tear down.
    ~S();
    /// A real method.
    void go();
};
)");
        EXPECT_EQ(first(m, "S::S"), nullptr);
        EXPECT_NE(first(m, "S::go"), nullptr);
    }

    TEST(DocComments, InlineBodiesDoNotLeakIntoTheNextDeclaration) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Doc for f.
    int f() const { int x = 0; if (x) { return 1; } return x; }
    int g() const;
};
)");
        ASSERT_NE(first(m, "S::f"), nullptr);
        EXPECT_EQ(first(m, "S::f")->doc, "Doc for f.");
        // g has no comment of its own and must not inherit f's.
        const DocEntryInfo *g = first(m, "S::g");
        if (g != nullptr) {
            EXPECT_TRUE(g->doc.empty());
        }
    }

    TEST(DocComments, AMemberAfterAnInlineBodyKeepsItsOwnComment) {
        // Regression: stepping past an inline body used to skip whitespace AND
        // comments, so the very next member's doc comment was consumed as
        // trailing noise. Every member after the first header-defined one then
        // lost its documentation — silently, and only in headers that define
        // their methods inline, which is most of them.
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Doc for f.
    int f() const { for (int i = 0; i < 3; ++i) {} return 1; }

    /// Doc for g.
    /// @return a number
    int g() const { return 2; }

    /// Doc for h.
    /// @param n how many
    void h(int n) { (void)n; }
};
)");
        ASSERT_NE(first(m, "S::f"), nullptr);
        EXPECT_EQ(first(m, "S::f")->doc, "Doc for f.");
        ASSERT_NE(first(m, "S::g"), nullptr);
        EXPECT_EQ(first(m, "S::g")->doc, "Doc for g.");
        EXPECT_EQ(first(m, "S::g")->returns, "a number");
        ASSERT_NE(first(m, "S::h"), nullptr);
        EXPECT_EQ(first(m, "S::h")->doc, "Doc for h.");
        ASSERT_EQ(first(m, "S::h")->params.size(), 1u);
        EXPECT_EQ(first(m, "S::h")->params[0].doc, "how many");
    }

    TEST(DocComments, AFieldAfterAnInlineBodyKeepsItsOwnComment) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Doc for f.
    int f() const { return 1; }
    /// Doc for v.
    int v = 0;
};
)");
        ASSERT_NE(first(m, "S::v"), nullptr);
        EXPECT_EQ(first(m, "S::v")->doc, "Doc for v.");
    }

    TEST(DocComments, StringsAndCommentsInsideBodiesAreNotParsed) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    /// Doc for f.
    const char *f() const { return "struct Fake { int trap; };"; }
};
)");
        EXPECT_EQ(first(m, "Fake::trap"), nullptr);
        EXPECT_NE(first(m, "S::f"), nullptr);
    }

    TEST(DocComments, PreprocessorLinesAreSkipped) {
        const DocMap m = harvest_doc_comments(R"(
#define MACRO(x) struct Trap { int x; };
struct S {
    /// Doc.
    void f();
};
)");
        EXPECT_EQ(first(m, "Trap::x"), nullptr);
        EXPECT_NE(first(m, "S::f"), nullptr);
    }

    TEST(DocComments, EnumsDoNotSwallowFollowingMembers) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    enum class Mode { Fast, Slow };
    /// Doc for f.
    void f();
};
)");
        ASSERT_NE(first(m, "S::f"), nullptr);
        EXPECT_EQ(first(m, "S::f")->doc, "Doc for f.");
    }

    TEST(DocComments, UsingAndTypedefDoNotConsumeTheNextComment) {
        const DocMap m = harvest_doc_comments(R"(
struct S {
    using Scalar = double;
    typedef int Index;
    /// Doc for f.
    void f();
};
)");
        ASSERT_NE(first(m, "S::f"), nullptr);
        EXPECT_EQ(first(m, "S::f")->doc, "Doc for f.");
    }

    TEST(DocComments, FreeFunctionsAreHarvested) {
        const DocMap        m = harvest_doc_comments(R"(
namespace api {
    /// Add two numbers.
    /// @param a the first
    /// @param b the second
    /// @return the sum
    int add(int a, int b = 1);
}
)");
        const DocEntryInfo *e = first(m, "api::add");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->doc, "Add two numbers.");
        EXPECT_EQ(e->returns, "the sum");
        ASSERT_EQ(e->params.size(), 2u);
        EXPECT_EQ(e->params[0].doc, "the first");
        EXPECT_EQ(e->params[1].default_text, "1");
    }

    TEST(DocComments, OverloadsAreKeptAsSeparateEntries) {
        const DocMap           m  = harvest_doc_comments(R"(
struct Grid {
    /// One index.
    double at(int i) const;
    /// Two indices.
    double at(int i, int j) const;
};
)");
        DocMap::const_iterator it = m.find("Grid::at");
        ASSERT_NE(it, m.end());
        ASSERT_EQ(it->second.size(), 2u);
        EXPECT_EQ(it->second[0].doc, "One index.");
        EXPECT_EQ(it->second[0].params.size(), 1u);
        EXPECT_EQ(it->second[1].doc, "Two indices.");
        EXPECT_EQ(it->second[1].params.size(), 2u);
    }

    TEST(DocComments, PlainCommentsAreNotDocumentation) {
        const DocMap        m = harvest_doc_comments(R"(
struct S {
    // an ordinary implementation note, not a doc comment
    void f();
};
)");
        const DocEntryInfo *e = first(m, "S::f");
        if (e != nullptr) {
            EXPECT_TRUE(e->doc.empty());
        }
    }

    TEST(DocComments, EmptySourceYieldsEmptyMap) {
        EXPECT_TRUE(harvest_doc_comments("").empty());
    }

} // namespace
