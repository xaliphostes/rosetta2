// Copyright (c) fmaerten@gmail.com
// License: MIT

// OpenAPI 3.1 generation backend — emits `openapi.json` describing the *same*
// HTTP surface the REST backend serves (it reuses the shared jsonable_* filters
// so the spec can't drift from the server). This is a pure-data backend: it
// renders the reflected IR as a JSON document, exploiting the annotation IR
// (range -> minimum/maximum, readonly -> readOnly, combobox -> enum,
// doc -> description). `openapi_doc()` is also consumed by the REST backend,
// which serves the spec at /openapi.json. Included by inline/generate.hxx
// *before* backends/inline/rest.hxx.

#pragma once

namespace rosetta {
    namespace backend {
        using namespace gen_detail; // shared render / IR helpers

        inline std::string oa_esc(const std::string &s) {
            std::string o;
            o.reserve(s.size());
            for (char c : s) {
                switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\t': o += "\\t"; break;
                case '\r': o += "\\r"; break;
                default: o += c;
                }
            }
            return o;
        }

        // Compact number: integral values without a trailing ".000000".
        inline std::string oa_num(double d) {
            if (d == static_cast<double>(static_cast<long long>(d))) {
                return std::to_string(static_cast<long long>(d));
            }
            std::string s = std::to_string(d);
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.') {
                s.pop_back();
            }
            return s;
        }

        inline std::string oa_join(const std::vector<std::string> &items) {
            std::string s;
            for (std::size_t i = 0; i < items.size(); ++i) {
                s += (i ? "," : "") + items[i];
            }
            return s;
        }

        // The bare type schema (no description/constraints). Object & enum types
        // become $refs into components/schemas.
        inline std::string oa_schema(const GenType &t, const GenContext &c) {
            if (t.kind == "object" || t.kind == "enum") {
                return t.object.empty()
                           ? "{}"
                           : "{\"$ref\":\"#/components/schemas/" +
                                 exposed_object_of(t, c) + "\"}";
            }
            if (t.kind == "vector") {
                return "{\"type\":\"array\",\"items\":" +
                       (t.element.empty() ? std::string("{}") : oa_schema(t.element.front(), c)) + "}";
            }
            if (t.kind == "number") {
                return t.integer ? "{\"type\":\"integer\"}" : "{\"type\":\"number\"}";
            }
            if (t.kind == "boolean") {
                return "{\"type\":\"boolean\"}";
            }
            if (t.kind == "string") {
                return "{\"type\":\"string\"}";
            }
            return "{}"; // void / unknown
        }

        // Add sibling keys to a schema object (legal in 3.1, even next to $ref).
        inline std::string oa_inject(const std::string &base,
                                     const std::vector<std::string> &extra) {
            if (extra.empty()) {
                return base;
            }
            std::string inner       = base.substr(0, base.size() - 1); // drop trailing }
            const bool  has_content = inner.size() > 1;                // more than just "{"
            for (std::size_t i = 0; i < extra.size(); ++i) {
                inner += ((i || has_content) ? "," : "") + extra[i];
            }
            return inner + "}";
        }

        // Field schema with annotation-derived constraints.
        inline std::string oa_field_schema(const GenField &f, const GenContext &c) {
            std::vector<std::string> extra;
            if (!f.doc.empty()) {
                extra.push_back("\"description\":\"" + oa_esc(f.doc) + "\"");
            }
            if (f.is_readonly) {
                extra.push_back("\"readOnly\":true");
            }
            if (f.range.has) {
                extra.push_back("\"minimum\":" + oa_num(f.range.min));
                extra.push_back("\"maximum\":" + oa_num(f.range.max));
            }
            if (!f.choices.empty()) {
                std::string e = "\"enum\":[";
                for (std::size_t i = 0; i < f.choices.size(); ++i) {
                    e += (i ? "," : "") + ("\"" + oa_esc(f.choices[i]) + "\"");
                }
                e += "]";
                extra.push_back(e);
            }
            return oa_inject(oa_schema(f.type, c), extra);
        }

        inline std::string oa_content(const std::string &schema) {
            return "{\"application/json\":{\"schema\":" + schema + "}}";
        }
        inline std::string oa_ok(const std::string &schema) {
            return "\"200\":{\"description\":\"OK\",\"content\":" + oa_content(schema) + "}";
        }
        // A method/function return: 204 for void, else 200 with the schema.
        inline std::string oa_return_responses(const GenType &ret, const GenContext &c) {
            if (ret.kind == "void") {
                return "\"204\":{\"description\":\"No Content\"}";
            }
            return oa_ok(oa_schema(ret, c));
        }
        // A harvested default argument as JSON, or "" when it is not expressible
        // there. `Quality::Fine` is a perfectly good C++ default and no JSON
        // value at all, so it does not become a lying `"default"` — it is
        // mentioned in the description instead (see oa_param_schema).
        inline std::string oa_default_json(const std::string &cpp) {
            if (cpp.empty()) {
                return {};
            }
            if (cpp == "true" || cpp == "false") {
                return cpp;
            }
            if (cpp.size() >= 2 && cpp.front() == '"' && cpp.back() == '"') {
                return "\"" + oa_esc(cpp.substr(1, cpp.size() - 2)) + "\"";
            }
            // A plain decimal number; a suffixed or hex literal ("0u", "0x10L")
            // is C++ spelling, not JSON, so it is left out.
            std::size_t i = (cpp[0] == '-' || cpp[0] == '+') ? 1 : 0;
            if (i >= cpp.size()) {
                return {};
            }
            bool dot = false;
            for (std::size_t j = i; j < cpp.size(); ++j) {
                if (cpp[j] == '.' && !dot) {
                    dot = true;
                    continue;
                }
                if (cpp[j] < '0' || cpp[j] > '9') {
                    return {};
                }
            }
            return cpp;
        }

        // One argument's schema, carrying what the header said about it: the
        // parameter's NAME as the title, its @param text as the description, and
        // its default. The wire shape is unchanged — the request body stays a
        // positional array, which is what the REST backend serves — so this is
        // purely what a reader (or Swagger UI) sees.
        inline std::string oa_param_schema(const GenParam &p, const GenContext &c) {
            std::vector<std::string> extra;
            if (!p.name.empty()) {
                extra.push_back("\"title\":\"" + oa_esc(p.name) + "\"");
            }
            std::string       desc = p.doc;
            const std::string def  = oa_default_json(p.default_text);
            if (def.empty() && !p.default_text.empty()) {
                desc += (desc.empty() ? "" : " ") + std::string("(default: ") + p.default_text + ")";
            } else if (p.has_default && p.default_text.empty()) {
                desc += (desc.empty() ? "" : " ") + std::string("(optional in C++)");
            }
            if (!desc.empty()) {
                extra.push_back("\"description\":\"" + oa_esc(desc) + "\"");
            }
            if (!def.empty()) {
                extra.push_back("\"default\":" + def);
            }
            return oa_inject(oa_schema(p.type, c), extra);
        }

        // Positional args as a JSON array (3.1 tuple via prefixItems).
        inline std::string oa_args_request(const std::vector<GenParam> &ps, const GenContext &c) {
            std::string items = "[";
            for (std::size_t i = 0; i < ps.size(); ++i) {
                items += (i ? "," : "") + oa_param_schema(ps[i], c);
            }
            items += "]";
            const std::string n = std::to_string(ps.size());
            return "\"requestBody\":{\"required\":true,\"content\":" +
                   oa_content("{\"type\":\"array\",\"prefixItems\":" + items + ",\"minItems\":" + n +
                              ",\"maxItems\":" + n + "}") +
                   "}";
        }

        constexpr std::string_view OA_ID_PARAM =
            "{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"integer\"}}";
        constexpr std::string_view OA_404 = "\"404\":{\"description\":\"Not Found\"}";

        // Build the whole OpenAPI 3.1 document (minified JSON).
        inline std::string openapi_doc(const GenContext &c) {
            const std::string id_param(OA_ID_PARAM);
            std::vector<std::string> paths;
            std::vector<std::string> schemas;

            for (const auto &k : c.classes) {
                const std::string tag = "\"tags\":[\"" + exposed_of(k) + "\"]";

                // POST /Class  -> create (default instance), returns {id}
                paths.push_back(
                    "\"/" + exposed_of(k) + "\":{\"post\":{\"summary\":\"Create " + exposed_of(k) +
                    "\"," + tag +
                    ",\"responses\":{\"200\":{\"description\":\"created\",\"content\":" +
                    oa_content("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}}}") +
                    "}}}}");

                // DELETE /Class/{id}
                paths.push_back("\"/" + exposed_of(k) + "/{id}\":{\"delete\":{\"summary\":\"Delete " +
                                exposed_of(k) + "\"," + tag + ",\"parameters\":[" + id_param +
                                "],\"responses\":{\"204\":{\"description\":\"No Content\"}," +
                                std::string(OA_404) + "}}}");

                // GET/PUT /Class/{id}/<field>
                for (const auto &f : k.fields) {
                    if (!jsonable_type(f.type)) {
                        continue;
                    }
                    std::string get_op = "\"get\":{\"summary\":\"Get " + f.name + "\"," + tag +
                                         ",\"parameters\":[" + id_param + "],\"responses\":{" +
                                         oa_ok(oa_field_schema(f, c)) + "," + std::string(OA_404) + "}}";
                    std::string put_resps = "\"204\":{\"description\":\"No Content\"}";
                    if (f.range.has) {
                        put_resps += ",\"400\":{\"description\":\"Out of range\"}";
                    }
                    if (f.is_readonly) {
                        put_resps += ",\"403\":{\"description\":\"Read-only\"}";
                    }
                    put_resps += "," + std::string(OA_404);
                    std::string put_op =
                        "\"put\":{\"summary\":\"Set " + f.name + "\"," + tag + ",\"parameters\":[" +
                        id_param + "],\"requestBody\":{\"required\":true,\"content\":" +
                        oa_content(oa_schema(f.type, c)) + "},\"responses\":{" + put_resps + "}}";
                    paths.push_back("\"/" + k.name + "/{id}/" + f.name + "\":{" + get_op + "," +
                                    put_op + "}");
                }

                // POST /Class/{id}/<method>  (or /Class/<method> for statics)
                for (const auto &m : k.methods) {
                    if (!jsonable_method(m)) {
                        continue;
                    }
                    const std::string path = m.is_static ? ("/" + k.name + "/" + m.name)
                                                          : ("/" + k.name + "/{id}/" + m.name);
                    std::string       params_arr =
                        m.is_static ? "" : ("\"parameters\":[" + id_param + "],");
                    std::string resps = oa_return_responses(m.ret, c);
                    if (!m.is_static) {
                        resps += "," + std::string(OA_404);
                    }
                    // The method's own documentation becomes the operation
                    // description; @return text is what the 200 response means,
                    // but oa_return_responses owns that string, so it joins the
                    // description rather than being dropped.
                    std::string odesc = m.doc;
                    if (!m.returns.empty()) {
                        odesc += (odesc.empty() ? "" : "\n\n");
                        odesc += "Returns: " + m.returns;
                    }
                    const std::string odesc_json =
                        odesc.empty() ? "" : ("\"description\":\"" + oa_esc(odesc) + "\",");
                    paths.push_back("\"" + path + "\":{\"post\":{\"summary\":\"" + m.name + "\"," +
                                    odesc_json + tag + "," + params_arr +
                                    oa_args_request(m.params, c) + ",\"responses\":{" + resps +
                                    "}}}");
                }

                // schema for the class
                std::string props;
                for (std::size_t i = 0; i < k.fields.size(); ++i) {
                    props += (i ? "," : "") + ("\"" + k.fields[i].name + "\":" +
                                               oa_field_schema(k.fields[i], c));
                }
                schemas.push_back("\"" + exposed_of(k) + "\":{\"type\":\"object\",\"properties\":{" +
                                  props +
                                  "}}");
            }

            // GET /Enum  -> { name: value, ... }
            for (const auto &e : c.enums) {
                paths.push_back(
                    "\"/" + exposed_of(e) + "\":{\"get\":{\"summary\":\"" + exposed_of(e) +
                    " values\",\"tags\":[\"enums\"],\"responses\":{" +
                    oa_ok("{\"type\":\"object\",\"additionalProperties\":{\"type\":\"integer\"}}") +
                    "}}}");
                std::string vals, desc;
                for (std::size_t i = 0; i < e.values.size(); ++i) {
                    vals += (i ? "," : "") + std::to_string(e.values[i].value);
                    desc += (i ? ", " : "") + e.values[i].name + "=" +
                            std::to_string(e.values[i].value);
                }
                schemas.push_back("\"" + exposed_of(e) + "\":{\"type\":\"integer\",\"enum\":[" + vals +
                                  "],\"description\":\"" + desc + "\"}");
            }

            // POST /<free function>
            for (const auto &f : c.functions) {
                if (!jsonable_function(f)) {
                    continue;
                }
                paths.push_back("\"/" + f.name + "\":{\"post\":{\"summary\":\"" + f.name +
                                "\",\"tags\":[\"functions\"]," +
                                (f.doc.empty() ? "" : "\"description\":\"" + oa_esc(f.doc) + "\",") +
                                oa_args_request(f.params, c) + ",\"responses\":{" +
                                oa_return_responses(f.ret, c) + "}}}");
            }

            return "{\"openapi\":\"3.1.0\",\"info\":{\"title\":\"" + c.lib +
                   "\",\"version\":\"1.0.0\",\"description\":\"Auto-generated by rosetta from "
                   "reflected C++ (REST backend).\"},\"paths\":{" +
                   oa_join(paths) + "},\"components\":{\"schemas\":{" + oa_join(schemas) + "}}}";
        }

        inline void OpenApi::emit(const GenContext &c) const {
            auto dir = c.out_dir / "openapi";
            write_file(dir / "openapi.json", openapi_doc(c));
            write_file(dir / "README.md", readme("openapi", c));
        }

    } // namespace backend
} // namespace rosetta
