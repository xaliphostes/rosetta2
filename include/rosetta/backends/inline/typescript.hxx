// Copyright (c) fmaerten@gmail.com
// License: MIT

// TypeScript generation backend — emits a `.d.ts` ambient module describing
// the bound classes (a companion to the node / wasm runtime bindings). This
// is a *pure-data* backend: it consumes the reflection IR that `generate`
// erases into GenClass (fields / methods / ctors) and renders text — no C++
// is compiled. Included by inline/generate.hxx.

#pragma once

namespace rosetta {
    namespace backend {
        using namespace gen_detail; // shared render / IR helpers

        // Render a neutral GenType as a TypeScript type expression. A class /
        // enum reference resolves against the bound-class list so it names the
        // EXPOSED class (manifest "expose" may rename it; two bound classes may
        // even share an unqualified C++ identifier). Unresolved objects keep the
        // reflected identifier, matching the previous behavior.
        inline std::string ts_type(const GenType &t, const GenContext &c) {
            if (t.kind == "number") {
                return "number";
            }
            if (t.kind == "boolean") {
                return "boolean";
            }
            if (t.kind == "string") {
                return "string";
            }
            if (t.kind == "void") {
                return "void";
            }
            // A shared_ptr is, to a script, the object it points at — the
            // reference count is a C++-side detail. Declaring "shared_ptr" (the
            // literal identifier the IR carries in `object`) would name a type
            // the .d.ts never defines.
            if (t.is_shared_ptr && !t.element.empty()) {
                return ts_type(t.element.front(), c);
            }
            if (t.kind == "object" || t.kind == "enum") {
                if (t.object.empty()) {
                    return "any";
                }
                for (const auto &k : c.classes) {
                    if (t.object_qualified.empty() ? (k.name == t.object)
                                                   : (qualified_of(k) == t.object_qualified)) {
                        return exposed_of(k);
                    }
                }
                return t.object;
            }
            if (t.kind == "vector") {
                return (t.element.empty() ? std::string("any")
                                          : ts_type(t.element.front(), c)) +
                       "[]";
            }
            if (is_adapted(t) && !t.element.empty()) {
                // A trait-registered foreign container marshals as an array of
                // its element in the opted-in runtime backends — and a matrix
                // as an array of those rows.
                return ts_type(t.element.front(), c) + (t.is_matrix ? "[][]" : "[]");
            }
            return "any"; // unknown (e.g. std::function, unsupported types)
        }

        // A parameter name that is safe to write in a `.d.ts`. The IR now
        // carries the identifier the C++ declaration used, and C++ happily names
        // a parameter `function`, `var` or `new` — each of which is a syntax
        // error in a TypeScript parameter list. Such a name falls back to the
        // positional form rather than breaking the whole declaration file.
        inline std::string ts_param_name(const std::string &name, std::size_t index) {
            static const char *reserved[] = {
                "break",   "case",     "catch",  "class",   "const",    "continue", "debugger",
                "default", "delete",   "do",     "else",    "enum",     "export",   "extends",
                "false",   "finally",  "for",    "function","if",       "import",   "in",
                "instanceof","new",    "null",   "return",  "super",    "switch",   "this",
                "throw",   "true",     "try",    "typeof",  "var",      "void",     "while",
                "with",    "yield",    "let",    "static",  "implements","interface","package",
                "private", "protected","public", "await",
            };
            for (const char *r : reserved) {
                if (name == r) {
                    return "arg" + std::to_string(index);
                }
            }
            return name.empty() ? "arg" + std::to_string(index) : name;
        }

        // Out-parameters (manifest "out_params") are not arguments: the caller
        // receives them, so they leave the parameter list and join the return.
        //
        // A parameter with a default argument is declared OPTIONAL (`x?: T`).
        // TypeScript requires every optional parameter to follow the required
        // ones, which C++ guarantees for defaults — but only along the run that
        // reaches the end: once an out-parameter is removed from the middle, or
        // a trailing defaulted parameter is dropped, a `?` earlier in the list
        // would not compile. So optionality is applied from the END backwards
        // and stops at the first parameter that does not have a default.
        inline std::string ts_params(const std::vector<GenParam> &ps, const GenContext &c) {
            std::vector<const GenParam *> kept;
            for (const auto &p : ps) {
                if (!is_out_param(p)) {
                    kept.push_back(&p);
                }
            }
            std::vector<bool> optional(kept.size(), false);
            for (std::size_t i = kept.size(); i-- > 0;) {
                if (!kept[i]->has_default) {
                    break;
                }
                optional[i] = true;
            }
            std::vector<std::string> parts;
            for (std::size_t i = 0; i < kept.size(); ++i) {
                parts.push_back(ts_param_name(kept[i]->name, i) + (optional[i] ? "?" : "") + ": " +
                                ts_type(kept[i]->type, c));
            }
            return join(parts, ", ");
        }

        // The TSDoc block for a documented member: the prose, then one `@param`
        // per documented argument, then `@returns`. Emitted only when there is
        // something to say, so an undocumented API keeps its clean shape.
        inline std::string ts_doc(const std::string &doc, const std::vector<GenParam> &ps,
                                  const std::string &returns, const std::string &indent) {
            bool any = !doc.empty() || !returns.empty();
            for (const auto &p : ps) {
                any = any || (!p.doc.empty() && !is_out_param(p));
            }
            if (!any) {
                return {};
            }
            // A comment must not be able to close itself early.
            auto safe = [](std::string s) {
                for (std::size_t i = s.find("*/"); i != std::string::npos; i = s.find("*/", i)) {
                    s.replace(i, 2, "*\\/");
                }
                return s;
            };
            // One " * " line per line of prose. A BLANK line is written as
            // " *" with no trailing space — the paragraph break has to survive,
            // but trailing whitespace in generated output does not.
            const auto doc_line = [&indent](std::string &out, const std::string &line) {
                out += indent + (line.empty() ? " *" : " * " + line) + "\n";
            };
            std::string out = indent + "/**\n";
            if (!doc.empty()) {
                std::string line;
                for (char ch : safe(doc)) {
                    if (ch == '\n') {
                        doc_line(out, line);
                        line.clear();
                        continue;
                    }
                    line += ch;
                }
                doc_line(out, line);
            }
            std::size_t i = 0;
            for (const auto &p : ps) {
                if (is_out_param(p)) {
                    continue;
                }
                if (!p.doc.empty()) {
                    out += indent + " * @param " + ts_param_name(p.name, i) + " " + safe(p.doc) +
                           "\n";
                }
                ++i;
            }
            if (!returns.empty()) {
                out += indent + " * @returns " + safe(returns) + "\n";
            }
            return out + indent + " */\n";
        }

        // The declared return: the type itself normally, and a TUPLE when the
        // signature has out-parameters — `[boolean, number[], number]` for
        // `bool get_doubles(const string&, vector<double>&, index_t&)`, matching
        // the array node and wasm hand back and the multiple values Lua does.
        inline std::string ts_return(const GenType &ret, const std::vector<GenParam> &ps,
                                     const GenContext &c) {
            std::vector<std::string> parts;
            if (ret.kind != "void") {
                parts.push_back(ts_type(ret, c));
            }
            for (const auto &p : ps) {
                if (is_out_param(p)) {
                    parts.push_back(ts_type(p.type, c));
                }
            }
            if (parts.empty()) {
                return "void";
            }
            if (parts.size() == 1 && !ret.kind.empty() && ret.kind != "void") {
                return parts[0]; // no out-parameters: unchanged
            }
            return "[" + join(parts, ", ") + "]";
        }

        inline void TypeScript::emit(const GenContext &c) const {
            std::string out = "// Generated by rosetta::generate — do not edit by hand.\n";
            out += "declare module \"" + c.lib + "\" {\n";

            for (const auto &e : c.enums) {
                out += "    export enum " + exposed_of(e) + " {\n";
                for (const auto &v : e.values) {
                    out += "        " + v.name + " = " + std::to_string(v.value) + ",\n";
                }
                out += "    }\n";
            }

            for (const auto &k : c.classes) {
                if (!k.brief.empty()) {
                    out += ts_doc(k.brief, {}, {}, "    ");
                }
                out += "    export class " + exposed_of(k) + " {\n";

                for (const auto &ct : k.ctors) {
                    out += "        constructor(" + ts_params(ct, c) + ");\n";
                }

                for (const auto &f : k.fields) {
                    if (is_adapted(f.type) && !adapt_ok(f.type)) {
                        coverage::note_skip_field("typescript", k, f, "sequence_not_adaptable",
                                                  "a registered sequence with no std::vector "
                                                  "boundary adapter — the runtime backends skip "
                                                  "it, so this declaration would over-promise");
                        continue; // not adaptable — the runtime backends skip it
                    }
                    if (f.type.kind == "object" &&
                        !(f.type.copy_constructible && f.type.copy_assignable)) {
                        // A non-copyable member object of a BOUND class is a
                        // read-only member-object property in the runtime
                        // backends (mesh.vertices aliasing the real store);
                        // anything else non-copyable stays hidden.
                        bool bound = false;
                        for (const auto &kk : c.classes) {
                            bound = bound || (f.type.object_qualified.empty()
                                                  ? kk.name == f.type.object
                                                  : qualified_of(kk) == f.type.object_qualified);
                        }
                        bool clashes = false;
                        for (const auto &m : k.methods) {
                            clashes = clashes || m.name == f.name;
                        }
                        if (bound && !clashes) {
                            if (!f.doc.empty()) {
                                out += "        /** " + f.doc + " */\n";
                            }
                            out += "        readonly " + f.name + ": " + ts_type(f.type, c) + ";\n";
                            coverage::note_bound_field("typescript", k, f);
                        } else {
                            coverage::note_skip_field(
                                "typescript", k, f, "unmarshalable_type",
                                bound ? "a non-copyable member object whose name collides with a "
                                        "method"
                                      : "a non-copyable member object of an unbound class");
                        }
                        continue;
                    }
                    if (!f.doc.empty()) {
                        out += "        /** " + f.doc + " */\n";
                    }
                    out += "        " + std::string(f.is_readonly ? "readonly " : "") + f.name +
                           ": " + ts_type(f.type, c) + ";\n";
                    coverage::note_bound_field("typescript", k, f);
                }

                for (const auto &m : k.methods) {
                    // Same visibility rule as the runtime backends: a
                    // non-copyable class return or a non-copyable by-value
                    // parameter is skipped there, so don't declare it here
                    // either. A signature touching a foreign sequence follows
                    // the adapter rule: declared iff adaptable, hidden
                    // otherwise.
                    //
                    // Overloads follow the NODE runtime rather than what
                    // TypeScript could express. TypeScript does support
                    // declaration overloads, but this .d.ts describes the N-API
                    // module, which binds only the first-declared entry — so
                    // declaring the others would promise the caller a method
                    // that is not there, which is worse than not declaring it.
                    if (!coverage::emit_overload(coverage::overloads::first_only, "typescript", k,
                                                 m)) {
                        continue;
                    }
                    bool visible;
                    // Which of the two gates rejected it, so the coverage
                    // report distinguishes "the sequence has no adapter" from
                    // "the signature names a type that cannot cross" -- the
                    // same two reasons the runtime backends record.
                    const bool seq = seq_touches(m);
                    if (seq) {
                        visible = seq_adaptable(m);
                    } else {
                        visible = !(m.ret.kind == "object" && !m.ret.copy_constructible);
                    }
                    for (const auto &p : m.params) {
                        visible = visible && !(p.type.kind == "object" && !p.is_ref &&
                                               !p.type.copy_constructible);
                    }
                    if (!visible) {
                        if (seq) {
                            coverage::note_skip("typescript", k, m, "sequence_not_adaptable",
                                                "a registered sequence in the signature has no "
                                                "std::vector boundary adapter for the runtime "
                                                "backend this .d.ts describes");
                        } else {
                            coverage::note_skip("typescript", k, m, "unmarshalable_signature",
                                                "a non-copyable class by value in the signature; "
                                                "the runtime backend skips it, so declaring it "
                                                "would promise a method that is not there");
                        }
                        continue;
                    }
                    out += ts_doc(m.doc, m.params, m.returns, "        ");
                    out += "        " + std::string(m.is_static ? "static " : "") + m.name + "(" +
                           ts_params(m.params, c) + "): " + ts_return(m.ret, m.params, c) + ";\n";
                    coverage::note_bound("typescript", k, m);
                }

                out += "    }\n";
            }

            for (const auto &f : c.functions) {
                // Free functions get the SAME visibility rule as methods above.
                // They had none, which made this the one place the .d.ts could
                // over-promise: a function the N-API module skips for an
                // unmarshalable type was still declared here, so the caller got
                // a compile-time green light for something not in the module.
                GenMethod probe;
                probe.ret    = f.ret;
                probe.params = f.params;
                bool visible;
                const bool seq = seq_touches(probe);
                if (seq) {
                    visible = seq_adaptable(probe);
                } else {
                    visible = !(f.ret.kind == "object" && !f.ret.copy_constructible);
                }
                for (const auto &p : f.params) {
                    visible = visible && !(p.type.kind == "object" && !p.is_ref &&
                                           !p.type.copy_constructible);
                }
                if (!visible) {
                    coverage::note_skip_function(
                        "typescript", f,
                        seq ? "sequence_not_adaptable" : "unmarshalable_signature",
                        seq ? "a registered sequence in the signature has no std::vector "
                              "boundary adapter for the runtime backend this .d.ts describes"
                            : "a non-copyable class by value in the signature; the runtime "
                              "backend skips it, so declaring it would promise a function "
                              "that is not there");
                    continue;
                }
                out += ts_doc(f.doc, f.params, f.returns, "    ");
                out += "    export function " + f.name + "(" + ts_params(f.params, c) +
                       "): " + ts_return(f.ret, f.params, c) + ";\n";
                coverage::note_bound_function("typescript", f);
            }

            out += "}\n";
            write_file(c.out_dir / "typescript" / (c.lib + ".d.ts"), out);
        }

    } // namespace backend
} // namespace rosetta
