// Copyright (c) fmaerten@gmail.com
// License: MIT

// --init: write a starter manifest. Without a source dir, the fully-commented
// example below; with one, a manifest pre-filled from a heuristic scan of its
// headers and sources (see init.h and the scan section further down).

#include "init.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// A fully-commented example manifest, emitted by `--init`. It exercises every
// commonly-used field — a `variables` declaration feeding the cpp26_* toolchain
// overrides (the case it was added for), a multi-entry user_include,
// rosetta_include, generator_name / module_name, user_sources,
// compile_definitions, build_type / optimization, doc_comments, a
// representative spread of targets, and one example class and one example
// function — so the user can delete what they don't need rather than hunt the
// docs for what exists.
static std::string render_example_manifest() {
    return R"JSON({
    "//": "Rosetta binding manifest. Edit the fields below to match your project.",
    "//paths": "All relative paths resolve from THIS file's directory.",

    "//variables": "Optional manifest-local variables, substituted as $NAME / ${NAME} into every string below, so a shared path prefix is written once. Ordered: a value may use the ones before it. $ENV{...} and any undeclared ${...} are CMake's and pass through untouched.",
    "variables": [
        { "name": "P2996", "value": "$ENV{HOME}/devs/c++/clang-p2996" }
    ],

    "//cpp26": "Optional C++26 / P2996 reflection toolchain (clang-p2996 build dir). The generator itself always needs it; of the targets, only \"rest\" does. Omit to fall back to the built-in defaults ($ENV{HOME}/devs/c++/clang-p2996/build).",
    "cpp26_root": "$P2996/build",
    "cpp26_cxx": "$P2996/build/bin/clang++",
    "cpp26_cc": "$P2996/build/bin/clang",
    "cpp26_lib": "$P2996/build/lib",

    "//include": "user_include is one path or an array of them; each is added to the bindings' include path.",
    "user_include": [
        "./src",
        "./extern/eigen-3.4.0"
    ],
    "rosetta_include": "./extern/rosetta/include",

    "//names": "generator_name is the driver tool / CMake target; module_name is the default per-backend module name when a target gives no explicit name.",
    "generator_name": "mylib",
    "module_name": "mylib",

    "//user_sources": "Optional .cpp (or .c) files compiled straight into every binding target. Use when the bound headers only DECLARE the API and the bodies live in these sources (rather than a pre-built library). Entries may be shell globs, e.g. \"./src/algorithms/*.cpp\"; a \"**\" component matches zero or more directories, so \"./src/**/*.cpp\" covers a whole tree. C sources make the generated CMakeLists enable_language(C) automatically.",
    "user_sources": [
        "./src/widget.cpp",
        "./src/algorithms/*.cpp"
    ],

    "//compile_definitions": "Optional preprocessor definitions (\"NAME\" or \"NAME=VALUE\") applied to the driver and every compiled binding target — e.g. a third-party lib's configuration switches such as XXX_USE_BUILTIN_DEPS or XXX_WITH_HLBFGS. Omit if unused.",
    "compile_definitions": [
        "MYLIB_SOME_SWITCH"
    ],

    "//build": "Optional build configuration for every compiled backend's generated CMakeLists. build_type is the default CMAKE_BUILD_TYPE (Debug | Release | RelWithDebInfo | MinSizeRel; -DCMAKE_BUILD_TYPE=... at configure time still wins). optimization is an explicit -O flag (-O0..-O3, -Os, -Oz, -Og, -Ofast) added after the build type's own flags, so it overrides their -O level. Omit either if unused.",
    "build_type": "Release",
    "optimization": "-O2",

    "//doc_comments": "Optional. true (the default) reads the Doxygen documentation and default arguments already in the bound headers -- /// and /** @param ... @return ... */ blocks -- and carries them into every backend, as Python docstrings + keyword arguments, TSDoc, Javadoc, C# XML docs, OpenAPI descriptions and the markdown/html reference. Annotations still win where both exist. Set false to generate from annotations alone.",
    "doc_comments": true,

    "//targets": "A target is a bare string (\"python\", uses module_name) or {\"lang\": ..., \"name\": ...}. The bindings are fully expanded, so they build with a stock compiler. Per-target extras: \"link_options\" (this target's link line only), \"out_dir\" (where the built artifact is copied), the runtime pins \"python\"/\"requires_python\"/\"napi_version\"/\"node_engine\", and — on python/nanobind only — \"wheel\"/\"wheel_dir\" to build a redistributable wheel on every --build.",
    "targets": [
        {"lang": "python",        "name": "mylib", "wheel": true, "wheel_dir": "./dist"},
        {"lang": "node",          "name": "mylib"},
        {"lang": "wasm", "name": "mylib"},
        {"lang": "typescript",    "name": "mylib"},
        "rest",
        "openapi",
        "markdown"
    ],

    "//defaults": "Optional shared defaults, factoring the repetition out of the class/function entries below. \"namespace\" qualifies every entry name that has no \"::\" of its own (a qualified name passes verbatim; a leading \"::\" pins an entry to the global namespace). \"header_dir\" is prepended to every entry header. Omit either if unused.",
    "namespace": "mylib",
    "header_dir": "mylib",

    "//classes": "Each class: its name and the header declaring it (\"mylib/widget.h\" here, via header_dir; \"Widget\" becomes \"mylib::Widget\" via namespace). \"name\" may be omitted to derive it from the header stem. \"header\" may also be a glob (\"*.h\", \"**/*.h\", with an optional \"exclude\") to bind every header it matches, one class per file named from its stem. \"annotations\" points at an optional out-of-line annotation JSON side-car.",
    "classes": [
        {"doc": "An example bound class.", "name": "Widget", "header": "widget.h"}
    ],

    "//functions": "Free (non-member) functions to bind. \"name\" may be qualified (other::compute) to override the default namespace; \"doc\" is an optional description. Overloaded or template function names cannot be bound (^^name is ill-formed for them).",
    "functions": [
        {"header": "widget.h", "name": "compute", "doc": "An example bound free function."}
    ]
}
)JSON";
}

// ---------------------------------------------------------------------------
// --init <src_dir>: heuristic source scan. Walks the directory for headers
// (.h/.hh/.hpp/.hxx) and sources (.cpp/.cxx/.cc), pulls class and free-function
// names out of the headers with a comment/string-aware token scan (NOT a real
// C++ parse — templates, overload sets and anonymous namespaces are skipped),
// and pre-fills the manifest's "classes" / "functions" / "user_sources"
// arrays. The result is a starting point the user reviews, not a finished
// manifest.
// ---------------------------------------------------------------------------

struct ScannedDecl {
    std::string name;   // qualified C++ name
    std::string header; // declaring header, relative to the scan root
};

struct ScanResult {
    std::vector<ScannedDecl> classes;
    std::vector<ScannedDecl> functions;
    std::vector<std::string> sources; // .cpp/.cxx/.cc, relative to the scan root
    std::vector<std::string> notes;   // skipped things, reported to the user
};

// Blank out comments, string/char literals (incl. raw strings) and
// preprocessor lines, so the token scan below never trips over `class` in a
// doc comment or a brace inside a string.
static std::string strip_noise(const std::string &src) {
    std::string out;
    out.reserve(src.size());
    enum State { Code, LineComment, BlockComment, Str, Chr, Pp, Raw };
    State       st            = Code;
    bool        at_line_start = true;
    std::string raw_close; // the )delim" closer of the raw string being skipped
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = i + 1 < src.size() ? src[i + 1] : '\0';
        switch (st) {
        case Code:
            if (c == '/' && n == '/') {
                st = LineComment;
                ++i;
            } else if (c == '/' && n == '*') {
                st = BlockComment;
                ++i;
            } else if (c == 'R' && n == '"' &&
                       !(i > 0 && (std::isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_'))) {
                std::size_t p = i + 2;
                std::string d;
                while (p < src.size() && src[p] != '(') {
                    d += src[p++];
                }
                raw_close = ")" + d + "\"";
                st        = Raw;
                i         = p;
                out += ' ';
            } else if (c == '"') {
                st = Str;
                out += ' ';
            } else if (c == '\'') {
                st = Chr;
                out += ' ';
            } else if (c == '#' && at_line_start) {
                st = Pp;
            } else {
                out += c;
            }
            break;
        case LineComment:
            if (c == '\n') {
                st = Code;
                out += '\n';
            }
            break;
        case BlockComment:
            if (c == '*' && n == '/') {
                st = Code;
                ++i;
                out += ' ';
            }
            break;
        case Str:
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                st = Code;
            }
            break;
        case Chr:
            if (c == '\\') {
                ++i;
            } else if (c == '\'') {
                st = Code;
            }
            break;
        case Pp:
            if (c == '\\' && n == '\n') {
                ++i; // line continuation: still the same directive
            } else if (c == '\n') {
                st = Code;
                out += '\n';
            }
            break;
        case Raw:
            if (c == ')' && src.compare(i, raw_close.size(), raw_close) == 0) {
                i += raw_close.size() - 1;
                st = Code;
            }
            break;
        }
        if (c == '\n') {
            at_line_start = true;
        } else if (!std::isspace((unsigned char)c)) {
            at_line_start = false;
        }
    }
    return out;
}

static bool is_ident(const std::string &t) {
    return !t.empty() && (std::isalpha((unsigned char)t[0]) || t[0] == '_');
}

// Split noise-stripped source into identifier / numeric-literal / one-char
// punctuation tokens — all the granularity the scan needs.
static std::vector<std::string> lex(const std::string &s) {
    std::vector<std::string> out;
    std::size_t              i = 0;
    while (i < s.size()) {
        const unsigned char c = s[i];
        if (std::isspace(c)) {
            ++i;
        } else if (std::isalpha(c) || c == '_') {
            std::size_t j = i + 1;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) {
                ++j;
            }
            out.push_back(s.substr(i, j - i));
            i = j;
        } else if (std::isdigit(c)) {
            std::size_t j = i + 1; // one token per numeric literal (incl. 1.5e-3f)
            while (j < s.size() &&
                   (std::isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '\'' ||
                    ((s[j] == '+' || s[j] == '-') &&
                     (s[j - 1] == 'e' || s[j - 1] == 'E' || s[j - 1] == 'p' || s[j - 1] == 'P')))) {
                ++j;
            }
            out.push_back(s.substr(i, j - i));
            i = j;
        } else {
            out.push_back(std::string(1, (char)c));
            ++i;
        }
    }
    return out;
}

// Index just past the token closing the group that opens at t[i] (== `open`).
// Unbalanced input yields t.size().
static std::size_t skip_balanced(const std::vector<std::string> &t, std::size_t i,
                                 const std::string &open, const std::string &close) {
    int depth = 0;
    for (; i < t.size(); ++i) {
        if (t[i] == open) {
            ++depth;
        } else if (t[i] == close && --depth == 0) {
            return i + 1;
        }
    }
    return i;
}

static bool in_list(std::initializer_list<const char *> l, const std::string &s) {
    for (const char *e : l) {
        if (s == e) {
            return true;
        }
    }
    return false;
}

// Scan one header's text for class definitions and free-function declarations
// at namespace scope. Namespaces and class bodies are entered (nested classes
// are found); every other brace group — function bodies, enums, initializers —
// is skipped wholesale.
static void scan_header(const std::string &text, const std::string &header, ScanResult &out) {
    const std::vector<std::string> t = lex(strip_noise(text));

    struct Scope {
        std::string name;
        bool        is_class;
        bool        hidden; // anonymous namespace / template class: don't record inside
    };
    std::vector<Scope> scopes;
    std::vector<int>   brace_pops; // how many scopes the next pending '}' closes
    bool               skip_next = false; // a template<...> head was just consumed

    auto qualify = [&](const std::string &n) {
        std::string q;
        for (const auto &s : scopes) {
            if (!s.name.empty()) {
                q += s.name + "::";
            }
        }
        return q + n;
    };
    auto hidden = [&] {
        for (const auto &s : scopes) {
            if (s.hidden) {
                return true;
            }
        }
        return false;
    };
    auto in_class = [&] {
        for (const auto &s : scopes) {
            if (s.is_class) {
                return true;
            }
        }
        return false;
    };

    for (std::size_t i = 0; i < t.size();) {
        const std::string &tk = t[i];

        if (tk == "template") {
            ++i;
            if (i < t.size() && t[i] == "<") {
                i = skip_balanced(t, i, "<", ">");
            }
            skip_next = true; // the class/function that follows is a template
            continue;
        }
        if (tk == "using" || tk == "typedef" || tk == "friend" || tk == "static_assert") {
            while (i < t.size() && t[i] != ";" && t[i] != "{") {
                ++i;
            }
            i = (i < t.size() && t[i] == "{") ? skip_balanced(t, i, "{", "}") : i + 1;
            skip_next = false;
            continue;
        }
        if (tk == "namespace") {
            std::vector<std::string> parts; // "a::b" arrives as idents a, b
            bool                     alias = false;
            ++i;
            while (i < t.size() && t[i] != "{" && t[i] != ";") {
                if (t[i] == "=") {
                    alias = true;
                } else if (is_ident(t[i])) {
                    parts.push_back(t[i]);
                }
                ++i;
            }
            if (i < t.size() && t[i] == "{" && !alias) {
                int n = 0;
                if (parts.empty()) { // anonymous namespace: internal linkage
                    scopes.push_back({"", false, true});
                    n = 1;
                } else {
                    for (const auto &p : parts) {
                        scopes.push_back({p, false, false});
                        ++n;
                    }
                }
                brace_pops.push_back(n);
            }
            if (i < t.size()) {
                ++i;
            }
            skip_next = false;
            continue;
        }
        if ((tk == "class" || tk == "struct") && !(i > 0 && t[i - 1] == "enum")) {
            // Gather the head up to '{' (definition), ';' (forward declaration)
            // or ':' (base clause ⇒ definition). Export macros, attributes and
            // alignas(...) may precede the name: it is the LAST identifier of
            // the head, minus a trailing `final`.
            std::vector<std::string> idents;
            std::size_t              j   = i + 1;
            bool                     def = false;
            while (j < t.size()) {
                const std::string &u = t[j];
                if (u == "{" || u == ";") {
                    def = (u == "{");
                    break;
                }
                if (u == ":") {
                    if (j + 1 < t.size() && t[j + 1] == ":") {
                        j += 2; // `::` of a qualified name — keep gathering
                        continue;
                    }
                    def = true; // base clause
                    break;
                }
                if (u == "(") {
                    j = skip_balanced(t, j, "(", ")");
                    continue;
                }
                if (u == "[") {
                    j = skip_balanced(t, j, "[", "]");
                    continue;
                }
                if (u == "<") {
                    j = skip_balanced(t, j, "<", ">");
                    continue;
                }
                if (is_ident(u)) {
                    idents.push_back(u);
                }
                ++j;
            }
            std::string name;
            if (!idents.empty()) {
                name = idents.back();
                if (name == "final" && idents.size() > 1) {
                    name = idents[idents.size() - 2];
                }
            }
            if (def) {
                if (!name.empty() && !skip_next && !hidden()) {
                    out.classes.push_back({qualify(name), header});
                }
                while (j < t.size() && t[j] != "{") { // past the base clause
                    if (t[j] == "(") {
                        j = skip_balanced(t, j, "(", ")");
                    } else if (t[j] == "<") {
                        j = skip_balanced(t, j, "<", ">");
                    } else {
                        ++j;
                    }
                }
                if (j < t.size()) { // enter the body: nested classes are found too
                    scopes.push_back({name, true, skip_next || name.empty()});
                    brace_pops.push_back(1);
                    i = j + 1;
                } else {
                    i = j;
                }
            } else {
                i = (j < t.size()) ? j + 1 : j; // past the ';'
            }
            skip_next = false;
            continue;
        }
        if (tk == "{") {
            if (i > 0 && t[i - 1] == "extern") { // extern "C" { ... } — transparent
                scopes.push_back({"", false, false});
                brace_pops.push_back(1);
                ++i;
            } else { // function body, enum body, initializer, ...
                i = skip_balanced(t, i, "{", "}");
            }
            continue;
        }
        if (tk == "}") {
            if (!brace_pops.empty()) {
                for (int k = 0; k < brace_pops.back(); ++k) {
                    scopes.pop_back();
                }
                brace_pops.pop_back();
            }
            ++i;
            continue;
        }
        if (tk == ";") {
            skip_next = false; // a template variable etc. ends here
            ++i;
            continue;
        }
        if (is_ident(tk) && i + 1 < t.size() && t[i + 1] == "(") {
            // Candidate free-function declaration `ret [ns::]name(args);` at
            // namespace scope. Walk back over the `ident::` chain, then demand
            // a return-type-ish token right before it — that is what separates
            // a declaration from a macro invocation or a call.
            const std::size_t        past = skip_balanced(t, i + 1, "(", ")");
            std::vector<std::string> chain{tk};
            std::size_t              start = i;
            while (start >= 3 && t[start - 1] == ":" && t[start - 2] == ":" &&
                   is_ident(t[start - 3])) {
                chain.insert(chain.begin(), t[start - 3]);
                start -= 3;
            }
            const std::string prev  = start > 0 ? t[start - 1] : std::string{};
            const std::string after = past < t.size() ? t[past] : std::string{};
            bool ok = !skip_next && !hidden() && !in_class();
            ok = ok && (is_ident(prev) || prev == ">" || prev == "*" || prev == "&") &&
                 !in_list({"return", "new", "delete", "throw", "case", "goto", "else", "do",
                           "operator", "co_return", "co_await", "co_yield", "enum", "typename"},
                          prev);
            ok = ok && !in_list({"if", "while", "for", "switch", "catch", "sizeof", "alignof",
                                 "alignas", "decltype", "noexcept", "static_assert", "typeid",
                                 "requires", "concept", "defined", "main", "int", "void", "bool",
                                 "char", "float", "double", "long", "short", "unsigned", "signed",
                                 "auto", "const", "constexpr"},
                                tk);
            // X::X(...) — an out-of-line constructor, not a free function
            ok = ok && !(chain.size() >= 2 && chain[chain.size() - 1] == chain[chain.size() - 2]);
            // declaration or definition, not an expression
            ok = ok && (after == ";" || after == "{" || after == "noexcept" || after == "-");
            if (ok) {
                std::string qn;
                for (const auto &p : chain) {
                    qn += (qn.empty() ? "" : "::") + p;
                }
                out.functions.push_back({qualify(qn), header});
            }
            i         = past;
            skip_next = false;
            continue;
        }
        ++i;
    }
}

// A source that defines main() must not land in user_sources (it would be
// compiled into every binding module and clash with its entry point).
static bool defines_main(const std::string &stripped) {
    const std::vector<std::string> t = lex(stripped);
    for (std::size_t i = 1; i + 1 < t.size(); ++i) {
        if (t[i] == "main" && t[i + 1] == "(" && is_ident(t[i - 1])) {
            return true;
        }
    }
    return false;
}

static ScanResult scan_directory(const fs::path &root) {
    ScanResult            r;
    std::vector<fs::path> headers, sources;
    std::error_code       ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path    p  = it->path();
        const std::string fn = p.filename().string();
        if (it->is_directory(ec)) {
            // don't descend into VCS dirs, build trees or generated output
            if (fn.empty() || fn[0] == '.' ||
                in_list({"build", "node_modules", "bindings", "gen", "generated", "extern",
                         "external", "third_party", "thirdparty", "vendor"},
                        fn)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string ext = p.extension().string();
        if (ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx") {
            headers.push_back(p);
        } else if (ext == ".cpp" || ext == ".cxx" || ext == ".cc") {
            sources.push_back(p);
        }
    }
    std::sort(headers.begin(), headers.end());
    std::sort(sources.begin(), sources.end());

    for (const auto &h : headers) {
        scan_header(read_file(h), fs::relative(h, root, ec).generic_string(), r);
    }
    for (const auto &s : sources) {
        const std::string rel = fs::relative(s, root, ec).generic_string();
        if (defines_main(strip_noise(read_file(s)))) {
            r.notes.push_back(rel + " defines main() — left out of \"user_sources\"");
            continue;
        }
        r.sources.push_back(rel);
    }

    // A class seen twice (e.g. both #if branches of a guard): keep the first.
    std::vector<ScannedDecl> classes;
    for (const auto &c : r.classes) {
        bool dup = false;
        for (const auto &k : classes) {
            dup = dup || k.name == c.name;
        }
        if (!dup) {
            classes.push_back(c);
        }
    }
    r.classes = std::move(classes);

    // The scan cannot pick an overload for you: it is a token scan, not a C++
    // parse, so it has no signature to write. Drop every overloaded name with a
    // note naming the manifest field that CAN say which one is meant.
    std::vector<ScannedDecl> fns;
    for (const auto &f : r.functions) {
        std::size_t count = 0;
        for (const auto &g : r.functions) {
            count += (g.name == f.name) ? 1 : 0;
        }
        bool seen = false;
        for (const auto &g : fns) {
            seen = seen || g.name == f.name;
        }
        if (count > 1) {
            const std::string note =
                f.name + " is overloaded — skipped (add it by hand with a \"signature\", "
                         "e.g. \"void(Mesh&, bool)\", to bind one overload)";
            if (std::find(r.notes.begin(), r.notes.end(), note) == r.notes.end()) {
                r.notes.push_back(note);
            }
        } else if (!seen) {
            fns.push_back(f);
        }
    }
    r.functions = std::move(fns);
    return r;
}

// `target` spelled relative to `base` when the relative form is readable
// (at most 3 "../" climbs), absolute otherwise. Manifest paths resolve from
// the manifest's own directory, so relative is the friendly default.
static std::string rel_or_abs(const fs::path &target, const fs::path &base) {
    std::error_code ec;
    const fs::path  abs = fs::absolute(target).lexically_normal();
    const fs::path  rel = fs::relative(abs, fs::absolute(base), ec);
    if (ec || rel.empty()) {
        return abs.generic_string();
    }
    int climbs = 0;
    for (const auto &part : rel) {
        climbs += (part == "..") ? 1 : 0;
    }
    if (climbs > 3) {
        return abs.generic_string();
    }
    std::string s = rel.generic_string();
    if (s[0] != '.' && s[0] != '/') {
        s = "./" + s;
    }
    return s;
}

// Best-effort guess at the rosetta include/ dir: walk up from this binary
// looking for include/rosetta/generate.h (the tool usually runs from inside
// the rosetta checkout). Empty when not found.
static std::string guess_rosetta_include(const char *argv0, const fs::path &manifest_dir) {
    std::error_code ec;
    fs::path        exe = fs::weakly_canonical(fs::path(argv0), ec);
    if (ec || exe.empty()) {
        return {};
    }
    for (fs::path d = exe.parent_path(); !d.empty() && d != d.root_path();
         d = d.parent_path()) {
        if (fs::exists(d / "include" / "rosetta" / "generate.h", ec)) {
            return rel_or_abs(d / "include", manifest_dir);
        }
    }
    return {};
}

// The manifest pre-filled from a scan. Same shape as the example manifest,
// but with the scanned classes / functions / sources in place of the
// placeholders (and only the fields the scan can say something about).
static std::string render_scanned_manifest(const fs::path &manifest_path,
                                           const fs::path &scan_root, const ScanResult &r,
                                           const std::string &rosetta_include) {
    const fs::path    base = fs::absolute(manifest_path).parent_path();
    const std::string inc  = rel_or_abs(scan_root, base);

    // Module name from the scanned directory's name, sanitized to an
    // identifier (a python/node module name cannot carry '-' or '.').
    fs::path norm = fs::absolute(scan_root).lexically_normal();
    if (!norm.has_filename()) { // "src/" — a trailing separator drops the name
        norm = norm.parent_path();
    }
    std::string name = norm.filename().string();
    for (char &c : name) {
        if (!std::isalnum((unsigned char)c) && c != '_') {
            c = '_';
        }
    }
    if (name.empty() || std::isdigit((unsigned char)name[0])) {
        name = "mylib";
    }

    // Factor shared spelling out of the entries, into the manifest-level
    // "namespace" / "header_dir" defaults load() applies back: the namespace
    // when every qualified scanned name opens with the same component, the
    // header dir when every header lives under the same first-level folder.
    std::string ns;
    {
        bool uniform = true;
        auto visit   = [&](const std::vector<ScannedDecl> &decls) {
            for (const auto &d : decls) {
                const auto p = d.name.find("::");
                if (p == std::string::npos) {
                    continue;
                }
                const std::string first = d.name.substr(0, p);
                if (ns.empty()) {
                    ns = first;
                } else if (ns != first) {
                    uniform = false;
                }
            }
        };
        visit(r.classes);
        visit(r.functions);
        if (!uniform) {
            ns.clear();
        }
    }
    std::string hdir;
    {
        bool uniform = !r.classes.empty() || !r.functions.empty();
        auto visit   = [&](const std::vector<ScannedDecl> &decls) {
            for (const auto &d : decls) {
                const auto p = d.header.find('/');
                const std::string first =
                    (p == std::string::npos) ? std::string{} : d.header.substr(0, p);
                if (first.empty() || (!hdir.empty() && hdir != first)) {
                    uniform = false;
                } else {
                    hdir = first;
                }
            }
        };
        visit(r.classes);
        visit(r.functions);
        if (!uniform) {
            hdir.clear();
        }
    }
    // "ns::Simple" shortens to "Simple"; a deeper name (nested class,
    // sub-namespace) stays fully qualified — load() takes any name with a
    // "::" verbatim, so it round-trips unchanged. A global-namespace name
    // (extern "C" functions, un-namespaced classes) must be pinned with the
    // leading-"::" escape, or load() would default-qualify it.
    auto short_name = [&](const std::string &n) {
        if (ns.empty()) {
            return n;
        }
        if (n.rfind(ns + "::", 0) == 0) {
            const std::string rest = n.substr(ns.size() + 2);
            if (rest.find("::") == std::string::npos) {
                return rest;
            }
            return n;
        }
        if (n.find("::") == std::string::npos) {
            return "::" + n; // global namespace, kept global on reload
        }
        return n;
    };
    auto short_header = [&](const std::string &h) {
        return hdir.empty() ? h : h.substr(hdir.size() + 1);
    };

    nlohmann::ordered_json j;
    j["//"] = "Pre-filled by `rosetta_gen --init` from a heuristic scan of " + inc +
              ". Review before building: drop what should not be bound; template classes, "
              "overloaded functions and anonymous namespaces were skipped.";
    j["user_include"] = inc;
    if (rosetta_include.empty()) {
        j["//rosetta_include"] = "FIXME: point this at the rosetta include/ directory.";
        j["rosetta_include"]   = "./extern/rosetta/include";
    } else {
        j["rosetta_include"] = rosetta_include;
    }
    j["generator_name"] = "generator_" + name;
    j["module_name"]    = name;
    if (!ns.empty()) {
        j["namespace"] = ns;
    }
    if (!hdir.empty()) {
        j["header_dir"] = hdir;
    }
    if (!r.sources.empty()) {
        auto &us = j["user_sources"] = nlohmann::ordered_json::array();
        for (const auto &s : r.sources) {
            us.push_back(inc == "." ? "./" + s : inc + "/" + s);
        }
    }
    j["//targets"] = "Pick the backends you need (python, node, wasm, typescript, "
                     "rest, openapi, markdown, ...).";
    j["targets"] = nlohmann::ordered_json::array({"python", "typescript"});
    auto &cls = j["classes"] = nlohmann::ordered_json::array();
    for (const auto &c : r.classes) {
        cls.push_back({{"name", short_name(c.name)}, {"header", short_header(c.header)}});
    }
    if (!r.functions.empty()) {
        auto &fns = j["functions"] = nlohmann::ordered_json::array();
        for (const auto &f : r.functions) {
            fns.push_back({{"name", short_name(f.name)}, {"header", short_header(f.header)}});
        }
    }
    return j.dump(4) + "\n";
}

// Write a starter manifest to `path` — the commented example, or, when
// `scan_dir` is given, one pre-filled from a scan of that directory. If a
// file is already there, warn and leave it untouched (never clobber a
// hand-written manifest).
int init_manifest(const fs::path &path, const fs::path &scan_dir, const char *argv0) {
    if (fs::exists(path)) {
        std::fprintf(stderr,
                     "rosetta_gen: %s already exists — not overwriting it.\n"
                     "             Remove it first (or pass a different path) to "
                     "regenerate it.\n",
                     path.string().c_str());
        return 1;
    }
    if (scan_dir.empty()) {
        write_file(path, render_example_manifest());
        std::fprintf(stderr, "wrote example manifest to %s\n", path.string().c_str());
        return 0;
    }
    const ScanResult r = scan_directory(scan_dir);
    for (const auto &n : r.notes) {
        std::fprintf(stderr, "rosetta_gen: note: %s\n", n.c_str());
    }
    if (r.classes.empty()) {
        std::fprintf(stderr,
                     "rosetta_gen: warning: no bindable classes found under %s — the "
                     "\"classes\" array is empty (add one before generating)\n",
                     scan_dir.string().c_str());
    }
    const std::string rosetta_inc =
        guess_rosetta_include(argv0, fs::absolute(path).parent_path());
    write_file(path, render_scanned_manifest(path, scan_dir, r, rosetta_inc));
    std::fprintf(stderr,
                 "wrote %s — %zu class(es), %zu function(s), %zu source file(s) from %s\n"
                 "review it before building: the scan is heuristic (templates, overloads "
                 "and macro-heavy headers are approximated)\n",
                 path.string().c_str(), r.classes.size(), r.functions.size(),
                 r.sources.size(), scan_dir.string().c_str());
    return 0;
}
