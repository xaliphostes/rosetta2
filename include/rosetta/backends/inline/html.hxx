// Copyright (c) fmaerten@gmail.com
// License: MIT

// HTML documentation backend — implementation. Like the Markdown backend, this
// is a *pure-data* backend: it consumes the reflection IR that `generate` erases
// into GenClass (fields / methods / ctors / enums / functions) and renders a
// single self-contained HTML document — no C++ is compiled. The same annotation
// surface the other backends understand is surfaced here: doc -> description,
// readonly -> a "read-only" tag, range -> a "range: lo..hi" tag, combobox ->
// a "choices: …" tag. Included by inline/generate.hxx (via backends/html.h).

#pragma once

namespace rosetta {
    namespace backend {
        using namespace gen_detail; // shared render / IR helpers

        // Minimal HTML text escaping for content / attribute-free text.
        inline std::string html_escape(std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (char ch : s) {
                switch (ch) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default:  out += ch; break;
                }
            }
            return out;
        }

        // Render a double as a clean number (drop a trailing ".0…" for integers).
        inline std::string html_num(double d) {
            if (d == static_cast<long long>(d)) {
                return std::to_string(static_cast<long long>(d));
            }
            std::string s = std::to_string(d);
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.') {
                s.pop_back();
            }
            return s;
        }

        // A readable, HTML-escaped type name (shares the markdown renderer's
        // logic: vectors as element[], otherwise the prettified C++ spelling).
        inline std::string html_type(const GenType &t) {
            return html_escape(readable_type(t));
        }

        inline std::string html_params(const std::vector<GenParam> &ps) {
            std::string s;
            for (std::size_t i = 0; i < ps.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                // Same reading as the markdown renderer: `name?: type` when the
                // parameter is optional but its default could not be recovered,
                // `name: type = value` when it could.
                s += html_escape(ps[i].name);
                if (ps[i].has_default && ps[i].default_text.empty()) {
                    s += "?";
                }
                s += ": " + html_type(ps[i].type);
                if (!ps[i].default_text.empty()) {
                    s += " = " + html_escape(ps[i].default_text);
                }
            }
            return s;
        }

        // The description cell for a field: its doc text plus annotation tags.
        inline std::string html_field_desc(const GenField &f) {
            std::string d = html_escape(f.doc);
            auto        tag = [&](const std::string &t) {
                if (!d.empty()) {
                    d += " ";
                }
                d += "<span class=\"tag\">" + t + "</span>";
            };
            if (f.is_readonly) {
                tag("read-only");
            }
            if (f.range.has) {
                tag("range: " + html_num(f.range.min) + ".." + html_num(f.range.max));
            }
            if (!f.choices.empty()) {
                std::string c;
                for (std::size_t i = 0; i < f.choices.size(); ++i) {
                    c += (i ? ", " : "") + html_escape(f.choices[i]);
                }
                tag("choices: " + c);
            }
            return d;
        }

        constexpr std::string_view HTML_STYLE = R"CSS(<style>
  :root { color-scheme: light dark; }
  body { font: 16px/1.5 system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
         max-width: 60rem; margin: 2rem auto; padding: 0 1rem; }
  h1, h2, h3 { line-height: 1.25; }
  h2 { border-bottom: 1px solid #8884; padding-bottom: .2rem; margin-top: 2.5rem; }
  code { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
         background: #8881; padding: .1em .35em; border-radius: 4px; }
  table { border-collapse: collapse; width: 100%; margin: .5rem 0 1rem; }
  th, td { border: 1px solid #8884; padding: .4rem .6rem; text-align: left;
           vertical-align: top; }
  th { background: #8882; }
  .tag { font-size: .85em; background: #4a90d922; border: 1px solid #4a90d955;
         border-radius: 4px; padding: .05em .4em; white-space: nowrap; }
  nav ul { columns: 2; }
  .sig { display: block; margin: .15rem 0; }
</style>)CSS";

        inline std::string Html::render(const GenContext &c) const {
            std::string out = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
            out += "<meta charset=\"utf-8\">\n";
            out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
            out += "<title>" + html_escape(c.lib) + " API reference</title>\n";
            out += std::string(HTML_STYLE) + "\n";
            out += "</head>\n<body>\n";
            out += "<!-- Generated by rosetta::generate — do not edit by hand. -->\n";
            out += "<h1><code>" + html_escape(c.lib) + "</code> API reference</h1>\n";

            // -------- contents --------
            out += "<nav>\n<h2>Contents</h2>\n<ul>\n";
            for (const auto &k : c.classes) {
                out += "  <li><a href=\"#" + html_escape(exposed_of(k)) + "\">" +
                       html_escape(exposed_of(k)) +
                       "</a></li>\n";
            }
            for (const auto &e : c.enums) {
                out += "  <li><a href=\"#" + html_escape(exposed_of(e)) + "\">" +
                       html_escape(exposed_of(e)) +
                       "</a> <em>(enum)</em></li>\n";
            }
            if (!c.functions.empty()) {
                out += "  <li><a href=\"#functions\">Functions</a></li>\n";
            }
            out += "</ul>\n</nav>\n";

            // -------- classes --------
            for (const auto &k : c.classes) {
                out += "\n<section id=\"" + html_escape(exposed_of(k)) + "\">\n";
                out += "<h2>" + html_escape(exposed_of(k)) + "</h2>\n";

                if (!k.ctors.empty()) {
                    out += "<h3>Constructors</h3>\n";
                    for (const auto &ct : k.ctors) {
                        out += "<code class=\"sig\">" + html_escape(exposed_of(k)) + "(" +
                               html_params(ct) + ")</code>\n";
                    }
                }

                if (!k.fields.empty()) {
                    out += "<h3>Fields</h3>\n<table>\n";
                    out += "<thead><tr><th>Name</th><th>Type</th><th>Description</th></tr></thead>\n";
                    out += "<tbody>\n";
                    for (const auto &f : k.fields) {
                        out += "<tr><td><code>" + html_escape(f.name) + "</code></td><td><code>" +
                               html_type(f.type) + "</code></td><td>" + html_field_desc(f) +
                               "</td></tr>\n";
                    }
                    out += "</tbody>\n</table>\n";
                }

                if (!k.methods.empty()) {
                    out += "<h3>Methods</h3>\n";
                    for (const auto &m : k.methods) {
                        out += "<code class=\"sig\">" +
                               std::string(m.is_static ? "static " : "") + html_escape(m.name) +
                               "(" + html_params(m.params) + ") → " + html_type(m.ret) + "</code>\n";
                        if (!m.doc.empty()) {
                            out += "<p>" + html_escape(m.doc) + "</p>\n";
                        }
                        // Per-parameter documentation as a definition list —
                        // the part a reader scans for.
                        bool any_pdoc = false;
                        for (const auto &p : m.params) {
                            any_pdoc = any_pdoc || !p.doc.empty();
                        }
                        if (any_pdoc) {
                            out += "<dl>\n";
                            for (const auto &p : m.params) {
                                if (p.doc.empty()) {
                                    continue;
                                }
                                out += "<dt><code>" + html_escape(p.name) + "</code></dt><dd>" +
                                       html_escape(p.doc) + "</dd>\n";
                            }
                            out += "</dl>\n";
                        }
                        if (!m.returns.empty()) {
                            out += "<p><em>Returns:</em> " + html_escape(m.returns) + "</p>\n";
                        }
                    }
                }
                out += "</section>\n";
            }

            // -------- enums --------
            for (const auto &e : c.enums) {
                out += "\n<section id=\"" + html_escape(exposed_of(e)) + "\">\n";
                out += "<h2>" + html_escape(exposed_of(e)) + " <em>(enum";
                if (!e.underlying.empty()) {
                    out += " : " + html_escape(e.underlying);
                }
                out += ")</em></h2>\n";
                out += "<table>\n<thead><tr><th>Name</th><th>Value</th></tr></thead>\n<tbody>\n";
                for (const auto &v : e.values) {
                    out += "<tr><td><code>" + html_escape(v.name) + "</code></td><td><code>" +
                           std::to_string(v.value) + "</code></td></tr>\n";
                }
                out += "</tbody>\n</table>\n</section>\n";
            }

            // -------- free functions --------
            if (!c.functions.empty()) {
                out += "\n<section id=\"functions\">\n<h2>Functions</h2>\n";
                for (const auto &f : c.functions) {
                    out += "<code class=\"sig\">" + html_escape(f.name) + "(" +
                           html_params(f.params) + ") → " + html_type(f.ret) + "</code>\n";
                    if (!f.doc.empty()) {
                        out += "<p>" + html_escape(f.doc) + "</p>\n";
                    }
                }
                out += "</section>\n";
            }

            out += "</body>\n</html>\n";
            return out;
        }

        inline void Html::emit(const GenContext &c) const {
            write_file(c.out_dir / "html" / (c.lib + ".html"), render(c));
        }

    } // namespace backend

    template <typename... Ts> inline std::string to_html(std::string lib) {
        return backend::Html{}.render(gen_detail::make_context<Ts...>(std::move(lib)));
    }

} // namespace rosetta
