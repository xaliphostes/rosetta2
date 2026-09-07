// Copyright (c) fmaerten@gmail.com
// License: MIT

// Google Test suite for MATCHING harvested doc comments onto reflected
// signatures — rosetta::gen_detail::apply_doc_comments.
//
// The harvester (tests/doc_comments.cpp) reads header TEXT and can be wrong:
// it is a scanner, not a compiler. This is the layer that decides what to
// believe. Its contract is narrow and worth stating plainly:
//
//   - it only ever FILLS a field that is empty, so an annotation the author
//     wrote for rosetta always outranks a comment in the header;
//   - it matches an overload by arity AND parameter names, and when two
//     harvested entries both fit it applies NEITHER, because a plausible
//     sentence on the wrong overload is worse than no sentence;
//   - it takes a default argument's spelling only where reflection
//     independently reported that the parameter has a default — that string
//     ends up in generated C++, where a wrong answer does not compile.
//
// The suite drives apply_doc_comments over hand-built IR, so it pins the rules
// rather than any particular backend's rendering.

#include <gtest/gtest.h>
#include <map>
#include <rosetta/generate.h>
#include <string>
#include <vector>

namespace {

    using rosetta::DocEntry;
    using rosetta::DocParam;
    using rosetta::GenClass;
    using rosetta::GenFunction;
    using rosetta::GenMethod;
    using rosetta::GenParam;

    using Docs = std::map<std::string, std::vector<DocEntry>>;

    GenParam param(const std::string &name, bool has_default = false) {
        GenParam p;
        p.name        = name;
        p.has_default = has_default;
        p.type.kind   = "number";
        return p;
    }

    GenMethod method(const std::string &name, std::vector<GenParam> ps) {
        GenMethod m;
        m.name   = name;
        m.params = std::move(ps);
        return m;
    }

    GenClass shape(std::vector<GenMethod> methods) {
        GenClass k;
        k.name       = "Shape";
        k.name_space = "geom";
        k.qualified  = "geom::Shape";
        k.methods    = std::move(methods);
        return k;
    }

    DocEntry fn_entry(const std::string &doc, const std::string &returns,
                      std::vector<DocParam> ps) {
        DocEntry e;
        e.doc         = doc;
        e.returns     = returns;
        e.params      = std::move(ps);
        e.is_function = true;
        return e;
    }

    // NOT named `apply`: the arguments are std types, so ADL would find
    // std::apply and try to unpack the map as a tuple.
    void apply_docs(std::vector<GenClass> &classes, const Docs &docs) {
        std::vector<GenFunction> none;
        rosetta::gen_detail::apply_doc_comments(classes, none, docs);
    }

    // ---- the ordinary case --------------------------------------------------

    TEST(DocApply, FillsMethodDocReturnAndParameterText) {
        std::vector<GenClass> ks{shape({method("area", {param("radius")})})};
        Docs docs{{"geom::Shape::area",
                   {fn_entry("The area.", "pi r squared", {{"radius", "the radius", ""}})}}};
        apply_docs(ks, docs);
        const GenMethod &m = ks[0].methods[0];
        EXPECT_EQ(m.doc, "The area.");
        EXPECT_EQ(m.returns, "pi r squared");
        EXPECT_EQ(m.params[0].doc, "the radius");
    }

    TEST(DocApply, TheUnqualifiedKeyAlsoMatches) {
        std::vector<GenClass> ks{shape({method("area", {})})};
        Docs                  docs{{"Shape::area", {fn_entry("The area.", "", {})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].doc, "The area.");
    }

    TEST(DocApply, ClassAndFieldTextLand) {
        std::vector<GenClass> ks{shape({})};
        rosetta::GenField     f;
        f.name = "radius";
        ks[0].fields.push_back(f);

        DocEntry cls;
        cls.doc = "A shape.";
        DocEntry fld;
        fld.doc = "The radius.";
        Docs docs{{"geom::Shape", {cls}}, {"geom::Shape::radius", {fld}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].brief, "A shape.");
        EXPECT_EQ(ks[0].fields[0].doc, "The radius.");
    }

    // ---- annotations outrank harvested text ---------------------------------

    TEST(DocApply, AnExistingDocIsNeverOverwritten) {
        std::vector<GenClass> ks{shape({method("area", {param("radius")})})};
        ks[0].methods[0].doc           = "From the rosetta::doc annotation.";
        ks[0].methods[0].params[0].doc = "annotated parameter";
        Docs docs{{"geom::Shape::area",
                   {fn_entry("From the header.", "", {{"radius", "harvested parameter", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].doc, "From the rosetta::doc annotation.");
        EXPECT_EQ(ks[0].methods[0].params[0].doc, "annotated parameter");
    }

    // ---- overloads ----------------------------------------------------------

    TEST(DocApply, OverloadsAreMatchedByArity) {
        std::vector<GenClass> ks{
            shape({method("at", {param("i")}), method("at", {param("i"), param("j")})})};
        Docs docs{
            {"geom::Shape::at",
             {fn_entry("One index.", "", {{"i", "the index", ""}}),
              fn_entry("Two indices.", "", {{"i", "the row", ""}, {"j", "the column", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].doc, "One index.");
        EXPECT_EQ(ks[0].methods[0].params[0].doc, "the index");
        EXPECT_EQ(ks[0].methods[1].doc, "Two indices.");
        EXPECT_EQ(ks[0].methods[1].params[0].doc, "the row");
        EXPECT_EQ(ks[0].methods[1].params[1].doc, "the column");
    }

    TEST(DocApply, OverloadsOfEqualArityAreMatchedByParameterNames) {
        std::vector<GenClass> ks{
            shape({method("set", {param("radius")}), method("set", {param("diameter")})})};
        Docs docs{{"geom::Shape::set",
                   {fn_entry("By radius.", "", {{"radius", "r", ""}}),
                    fn_entry("By diameter.", "", {{"diameter", "d", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].doc, "By radius.");
        EXPECT_EQ(ks[0].methods[1].doc, "By diameter.");
    }

    TEST(DocApply, AnAmbiguousOverloadGroupDocumentsNeither) {
        // Same arity, and nothing in the names tells the two apart. Attaching
        // either sentence would be a coin flip printed as documentation.
        std::vector<GenClass> ks{
            shape({method("at", {param("arg0")}), method("at", {param("arg0")})})};
        Docs docs{
            {"geom::Shape::at",
             {fn_entry("First.", "", {{"", "", ""}}), fn_entry("Second.", "", {{"", "", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_TRUE(ks[0].methods[0].doc.empty());
        EXPECT_TRUE(ks[0].methods[1].doc.empty());
    }

    TEST(DocApply, AStaleSignatureIsNotMatched) {
        // The header the harvest came from is out of step with what reflection
        // reports — a different arity. Nothing is applied.
        std::vector<GenClass> ks{shape({method("area", {param("radius")})})};
        Docs                  docs{{"geom::Shape::area", {fn_entry("The area.", "", {})}}};
        apply_docs(ks, docs);
        EXPECT_TRUE(ks[0].methods[0].doc.empty());
    }

    // ---- default arguments --------------------------------------------------

    TEST(DocApply, DefaultSpellingLandsOnlyWhereReflectionSaysThereIsOne) {
        std::vector<GenClass> ks{
            shape({method("outline", {param("segments", /*has_default=*/true),
                                      param("mode", /*has_default=*/false)})})};
        // The harvest claims a default for BOTH. Reflection says only the first
        // has one; the second is a mis-read and must not reach generated C++.
        Docs docs{
            {"geom::Shape::outline",
             {fn_entry("Outline.", "", {{"segments", "", "32"}, {"mode", "", "Mode::Fast"}})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].params[0].default_text, "32");
        EXPECT_TRUE(ks[0].methods[0].params[1].default_text.empty());
    }

    TEST(DocApply, ADeclinedDefaultLeavesTheParameterMerelyOptional) {
        // has_default is true (reflection), default_text stays empty (the
        // harvester refused the expression). Backends must still be able to say
        // "optional" — that is what has_default is for.
        std::vector<GenClass> ks{shape({method("f", {param("p", /*has_default=*/true)})})};
        Docs                  docs{{"geom::Shape::f", {fn_entry("F.", "", {{"p", "", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_TRUE(ks[0].methods[0].params[0].default_text.empty());
        EXPECT_TRUE(ks[0].methods[0].params[0].has_default);
    }

    // ---- names ---------------------------------------------------------------

    TEST(DocApply, APositionalNameIsReplacedByTheDeclaredOne) {
        // The overload-selection path decomposes a function TYPE and has no
        // names; the header does. This is where they meet.
        std::vector<GenClass> ks{shape({method("f", {param("arg0"), param("arg1")})})};
        Docs                  docs{
            {"geom::Shape::f", {fn_entry("F.", "", {{"width", "", ""}, {"height", "", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].params[0].name, "width");
        EXPECT_EQ(ks[0].methods[0].params[1].name, "height");
    }

    TEST(DocApply, AReflectedNameIsNeverReplaced) {
        std::vector<GenClass> ks{shape({method("f", {param("radius")})})};
        Docs                  docs{{"geom::Shape::f", {fn_entry("F.", "", {{"radius", "", ""}})}}};
        apply_docs(ks, docs);
        EXPECT_EQ(ks[0].methods[0].params[0].name, "radius");
    }

    // ---- free functions ------------------------------------------------------

    TEST(DocApply, FreeFunctionsAreMatchedByQualifiedName) {
        GenFunction f;
        f.name      = "distance";
        f.qualified = "geom::distance";
        f.params    = {param("a"), param("b")};

        std::vector<GenClass>    ks;
        std::vector<GenFunction> fs{f};
        const Docs               docs{
            {"geom::distance",
             {fn_entry("The distance.", "how far", {{"a", "first", ""}, {"b", "second", ""}})}}};
        rosetta::gen_detail::apply_doc_comments(ks, fs, docs);
        EXPECT_EQ(fs[0].doc, "The distance.");
        EXPECT_EQ(fs[0].returns, "how far");
        EXPECT_EQ(fs[0].params[0].doc, "first");
        EXPECT_EQ(fs[0].params[1].doc, "second");
    }

    TEST(DocApply, AManifestDocOutranksTheHarvestedOne) {
        GenFunction f;
        f.name      = "distance";
        f.qualified = "geom::distance";
        f.doc       = "From the manifest.";

        std::vector<GenClass>    ks;
        std::vector<GenFunction> fs{f};
        const Docs               docs{{"geom::distance", {fn_entry("From the header.", "", {})}}};
        rosetta::gen_detail::apply_doc_comments(ks, fs, docs);
        EXPECT_EQ(fs[0].doc, "From the manifest.");
    }

    TEST(DocApply, AnEmptyHarvestChangesNothing) {
        std::vector<GenClass> ks{shape({method("area", {param("radius")})})};
        apply_docs(ks, {});
        EXPECT_TRUE(ks[0].methods[0].doc.empty());
        EXPECT_EQ(ks[0].methods[0].params[0].name, "radius");
    }

} // namespace
