// Copyright (c) fmaerten@gmail.com
// License: MIT

// load(): manifest.json -> Manifest (structs and field docs in manifest.h),
// plus the shell-glob expansion `user_sources` patterns go through.

#include "manifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

// True if `s` carries shell-glob magic (so it must be expanded, not used as a
// literal path). Matches POSIX glob's special characters.
static bool is_glob_pattern(const std::string &s) {
    return s.find_first_of("*?[") != std::string::npos;
}

// Match one path component against a glob component: `*`, `?` and `[...]`
// (with `!`/`^` negation and `a-z` ranges), POSIX-glob style. Hand-rolled so
// the tool builds on Windows too (no <glob.h>/fnmatch there); iterative with
// single-`*` backtracking.
static bool glob_match(const std::string &pat, const std::string &str) {
    std::size_t p = 0, s = 0;
    std::size_t star_p = std::string::npos, star_s = 0;
    // like POSIX glob, a wildcard never matches a leading dot
    if (!str.empty() && str[0] == '.' && !pat.empty() && pat[0] != '.') {
        return false;
    }
    while (s < str.size()) {
        bool step = false;
        if (p < pat.size() && pat[p] == '[') {
            std::size_t q   = p + 1;
            bool        neg = false;
            if (q < pat.size() && (pat[q] == '!' || pat[q] == '^')) {
                neg = true;
                ++q;
            }
            bool       hit   = false;
            bool       first = true;
            const char c     = str[s];
            while (q < pat.size() && (pat[q] != ']' || first)) {
                first = false;
                if (q + 2 < pat.size() && pat[q + 1] == '-' && pat[q + 2] != ']') {
                    hit = hit || (pat[q] <= c && c <= pat[q + 2]);
                    q += 3;
                } else {
                    hit = hit || (pat[q] == c);
                    ++q;
                }
            }
            if (q < pat.size() && hit != neg) { // q at ']' (unterminated set never matches)
                p    = q + 1;
                ++s;
                step = true;
            }
        } else if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p;
            ++s;
            step = true;
        } else if (p < pat.size() && pat[p] == '*') {
            star_p = p++;
            star_s = s;
            step   = true;
        }
        if (!step) {
            if (star_p == std::string::npos) {
                return false;
            }
            p = star_p + 1; // give the last '*' one more character
            s = ++star_s;
        }
    }
    while (p < pat.size() && pat[p] == '*') {
        ++p;
    }
    return p == pat.size();
}

// Expand a shell glob (relative to `base`) into the matching files, sorted for
// reproducible output. Used by `user_sources` so a manifest can say
// "src/algorithms/*.cpp" instead of listing every file. Expands `*`, `?` and
// `[...]` within a path component, and a component that is exactly `**`
// matches ZERO or more directories (bash-globstar style): "src/**/*.cxx"
// covers src/a.cxx and src/algos/deep/b.cxx alike, replacing one line per
// subdirectory. Like the other wildcards, `**` does not enter dot-dirs. A
// pattern that matches nothing yields an empty list (the caller warns).
// std::filesystem-based so it behaves identically on POSIX and Windows.
static std::vector<fs::path> expand_glob(const fs::path &base, const std::string &pattern) {
    const fs::path full = fs::absolute(base / fs::path(pattern)).lexically_normal();

    std::vector<fs::path> frontier{full.root_path()};
    for (const auto &part : full.relative_path()) {
        const std::string     comp = part.string();
        std::vector<fs::path> next;
        if (comp == "**") {
            // Zero or more directories: every frontier dir itself, plus every
            // non-hidden directory anywhere below it.
            for (const auto &dir : frontier) {
                std::error_code ec;
                if (!fs::is_directory(dir, ec)) {
                    continue;
                }
                next.push_back(dir);
                for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
                     it.increment(ec)) {
                    std::error_code ec2;
                    if (!it->is_directory(ec2)) {
                        continue;
                    }
                    const std::string name = it->path().filename().string();
                    if (!name.empty() && name[0] == '.') {
                        it.disable_recursion_pending();
                        continue;
                    }
                    next.push_back(it->path());
                }
            }
            frontier = std::move(next);
            continue;
        }
        for (const auto &dir : frontier) {
            std::error_code ec;
            if (!is_glob_pattern(comp)) {
                fs::path p = dir / part;
                if (fs::exists(p, ec)) {
                    next.push_back(std::move(p));
                }
                continue;
            }
            for (const auto &entry : fs::directory_iterator(dir, ec)) {
                if (glob_match(comp, entry.path().filename().string())) {
                    next.push_back(entry.path());
                }
            }
        }
        frontier = std::move(next);
        if (frontier.empty()) {
            break;
        }
    }

    std::vector<fs::path> out;
    for (const auto &p : frontier) {
        out.push_back(fs::weakly_canonical(p));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Fold an arbitrary directory name into something usable as a C++ / Python /
// Lua identifier: every character outside [A-Za-z0-9_] becomes '_', and a
// leading digit gains one. Used only where a name is DERIVED rather than
// written down — see generator_name in load().
static std::string identifier_from(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        out.push_back((std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_');
    }
    if (out.empty()) {
        return "bindings";
    }
    if (std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

// Deep-merge a preset fragment into the manifest, with the MANIFEST winning
// every conflict it can express:
//
//   * a key the manifest does not have  -> taken from the preset
//   * arrays present in both            -> preset entries first, manifest's after
//   * objects present in both           -> merged the same way, recursively
//   * a scalar the manifest spells out  -> kept; the preset never overrides it
//
// The array rule is what makes "module_init" compose: the preset contributes
// the caster include and the manifest keeps its own headers and statements,
// rather than one silently replacing the other.
//
// `classes` and `functions` are deliberately NOT merged here — see the preset
// block in load() for why they are parsed separately.
static void merge_preset(json &dst, const json &src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        const std::string &k = it.key();
        if (k.rfind("//", 0) == 0 || k == "classes" || k == "functions") {
            continue;
        }
        if (!dst.contains(k)) {
            dst[k] = it.value();
            continue;
        }
        json &d = dst.at(k);
        if (d.is_array() && it->is_array()) {
            json merged = *it;
            for (const auto &e : d) {
                merged.push_back(e);
            }
            d = std::move(merged);
        } else if (d.is_object() && it->is_object()) {
            merge_preset(d, *it);
        }
        // else: the manifest said something concrete. It wins.
    }
}

// --- "variables" -----------------------------------------------------------
//
// A shared path prefix written once instead of once per field:
//
//   "variables": [
//     { "name": "P2996", "value": "$ENV{HOME}/devs/c++/clang-p2996" }
//   ],
//   "cpp26_root": "$P2996/build",
//   "cpp26_cxx":  "${P2996}/build/bin/clang++"
//
// The substitution runs over every string in the document before any field is
// read, so a variable works anywhere a string does — not only in the toolchain
// paths that motivated it.
//
// One rule keeps this from colliding with the two OTHER dollar notations a
// manifest already carries, both of which belong to CMake and must reach it
// untouched: only a name this file DECLARED is substituted. `$ENV{HOME}` is
// skipped outright, and an undeclared `${CMAKE_SOURCE_DIR}` is left exactly as
// written. The price of that leniency is that a misspelt `$P2996x` passes
// through silently rather than failing — `${P2996}x` is how a variable abuts
// text, and the braced form is worth preferring for that reason.
//
// Declarations are an ARRAY rather than an object because they are ORDERED: a
// declaration may use the variables declared before it, and a JSON object has
// no order to lean on.
using Vars = std::vector<std::pair<std::string, std::string>>;

static const std::string *find_var(const Vars &vars, const std::string &name) {
    for (const auto &v : vars) {
        if (v.first == name) {
            return &v.second;
        }
    }
    return nullptr;
}

// Substitute `$NAME` / `${NAME}` for every declared NAME in one string.
static std::string expand_vars(const std::string &s, const Vars &vars) {
    if (vars.empty() || s.find('$') == std::string::npos) {
        return s;
    }
    const auto is_var_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '$') {
            out += s[i++];
            continue;
        }
        // "$ENV{...}" is CMake's own, expanded at ITS configure time: copied
        // through whole, so a name inside it is never mistaken for one of ours.
        if (s.compare(i, 5, "$ENV{") == 0) {
            const std::size_t close = s.find('}', i + 5);
            const std::size_t end   = (close == std::string::npos) ? s.size() : close + 1;
            out.append(s, i, end - i);
            i = end;
            continue;
        }
        std::size_t begin  = i + 1;
        const bool  braced = begin < s.size() && s[begin] == '{';
        if (braced) {
            ++begin;
        }
        std::size_t end = begin;
        while (end < s.size() && is_var_char(s[end])) {
            ++end;
        }
        std::size_t next = end;
        if (braced) {
            if (end >= s.size() || s[end] != '}') { // unterminated: not a reference
                out += s[i++];
                continue;
            }
            next = end + 1;
        }
        const std::string *val = (end > begin) ? find_var(vars, s.substr(begin, end - begin))
                                               : nullptr;
        if (val == nullptr) { // undeclared — CMake's, or plain text
            out += s[i++];
            continue;
        }
        out += *val;
        i = next;
    }
    return out;
}

// The same, over every string of a JSON document (values only — a key is a
// field name, never a path).
static void expand_vars_in(json &v, const Vars &vars) {
    if (v.is_string()) {
        v = expand_vars(v.get<std::string>(), vars);
    } else if (v.is_array() || v.is_object()) {
        for (auto &e : v) {
            expand_vars_in(e, vars);
        }
    }
}

// Read "variables", expanding each value against the declarations before
// it. Names are C identifiers, so `$P2996` has one obvious end.
static Vars read_variables(const json &j) {
    Vars vars;
    if (!j.contains("variables")) {
        return vars;
    }
    const json &decls = j.at("variables");
    if (!decls.is_array()) {
        throw std::runtime_error(
            "\"variables\" must be an ARRAY of {\"name\": ..., \"value\": ...} objects "
            "(the order is what lets one declaration use another)");
    }
    for (const auto &d : decls) {
        if (!d.is_object() || !d.contains("name") || !d.contains("value") ||
            !d.at("name").is_string() || !d.at("value").is_string()) {
            throw std::runtime_error("each \"variables\" entry must be "
                                     "{\"name\": \"NAME\", \"value\": \"...\"} with both strings");
        }
        const std::string name = d.at("name").get<std::string>();
        const bool        ok =
            !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) != 0 ||
                              name[0] == '_') &&
            std::all_of(name.begin(), name.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
            });
        if (!ok) {
            throw std::runtime_error("\"variables\" name \"" + name +
                                     "\" is not an identifier (letters, digits and _, "
                                     "not starting with a digit)");
        }
        if (name == "ENV") {
            throw std::runtime_error("\"variables\" cannot declare \"ENV\": $ENV{...} is "
                                     "CMake's environment lookup, passed through untouched");
        }
        if (find_var(vars, name) != nullptr) {
            throw std::runtime_error("\"variables\" declares \"" + name + "\" twice");
        }
        vars.emplace_back(name, expand_vars(d.at("value").get<std::string>(), vars));
    }
    return vars;
}

Manifest load(const fs::path &manifest_path) {
    std::ifstream in(manifest_path);
    if (!in) {
        throw std::runtime_error("cannot open " + manifest_path.string());
    }
    // Tolerate // and /* */ comments in the manifest.
    json j = json::parse(in, /*cb=*/nullptr, /*allow_exceptions=*/true,
                         /*ignore_comments=*/true);

    // Manifest-local variables, substituted into the whole document before a
    // single field is read — including `user_include` and `rosetta_include`
    // just below, which is why this comes first. See read_variables above.
    if (j.contains("variables")) {
        const Vars vars = read_variables(j);
        j.erase("variables"); // done its work; nothing downstream knows it
        expand_vars_in(j, vars);
    }

    Manifest       m;
    const fs::path base = fs::absolute(manifest_path).parent_path();

    // `user_include` is either a single string ("../my_lib") or an array of
    // them (["../my_lib", "../shared"]). Each is resolved relative to the
    // manifest (or taken absolute) and added to the bindings' include path.
    {
        const auto &ui = j.at("user_include");
        auto        add = [&](const std::string &p) {
            m.user_include.push_back(fs::weakly_canonical(base / fs::path(p)));
        };
        if (ui.is_array()) {
            if (ui.empty()) {
                throw std::runtime_error("\"user_include\" array must not be empty");
            }
            for (const auto &e : ui) {
                add(e.get<std::string>());
            }
        } else {
            add(ui.get<std::string>());
        }
    }
    m.rosetta_include =
        fs::weakly_canonical(base / fs::path(j.at("rosetta_include").get<std::string>()));

    // `preset` (a name, or an array of them) pulls in a JSON fragment shipped
    // under <rosetta_include>/rosetta/presets/<name>.json. A preset describes a
    // FIXED binding surface — rosetta's own reflection API, say — so a manifest
    // that uses one states only what is specific to its project: where its
    // headers and sources live, which targets to emit, what to run at load.
    //
    // `classes` / `functions` are held aside rather than merged into `j`,
    // because they must be parsed with a pristine EntryCtx: the manifest's own
    // `namespace` and `header_dir` defaults describe the USER's tree and must
    // not be prepended to a preset's headers. Everything else deep-merges,
    // manifest-wins — see merge_preset above.
    json preset = json::object();
    if (j.contains("preset")) {
        std::vector<std::string> names;
        if (j.at("preset").is_array()) {
            for (const auto &e : j.at("preset")) {
                names.push_back(e.get<std::string>());
            }
        } else {
            names.push_back(j.at("preset").get<std::string>());
        }
        for (const std::string &n : names) {
            const fs::path p = m.rosetta_include / "rosetta" / "presets" / (n + ".json");
            std::ifstream  pin(p);
            if (!pin) {
                throw std::runtime_error("unknown \"preset\": \"" + n + "\" (looked for " +
                                         p.string() + ")");
            }
            json pj = json::parse(pin, /*cb=*/nullptr, /*allow_exceptions=*/true,
                                  /*ignore_comments=*/true);
            merge_preset(j, pj);
            for (const char *key : {"classes", "functions"}) {
                if (pj.contains(key)) {
                    for (const auto &e : pj.at(key)) {
                        preset[key].push_back(e);
                    }
                }
            }
        }
        // A preset binds headers that live under rosetta_include, so it has to
        // be on the TARGET's include path too — not just the generator's. Every
        // other binding is reflection-free by construction and has no reason to
        // see rosetta's headers, which is why this is not the default.
        if (std::find(m.user_include.begin(), m.user_include.end(), m.rosetta_include) ==
            m.user_include.end()) {
            m.user_include.push_back(m.rosetta_include);
        }
    }

    // `generator_name` is optional; falls back to the manifest's parent
    // directory name. It names the emitted driver source (<name>.cpp) and is
    // the last-resort default for `module_name` — which is why the DERIVED form
    // is sanitised and the explicit one is not. A directory called
    // "my-cool-lib" would otherwise reach a backend as the module name and emit
    // PYBIND11_MODULE(my-cool-lib, m), a syntax error a long way from its
    // cause. An author who spells the key out gets what they wrote.
    m.generator_name = j.contains("generator_name")
                           ? j.at("generator_name").get<std::string>()
                           : identifier_from(base.filename().string());

    // `module_name` is optional too; the default binding module name when a
    // target gives no `name`. Falls back to `generator_name`.
    const std::string module_name =
        j.contains("module_name") ? j.at("module_name").get<std::string>() : m.generator_name;

    // Eleven languages once carried an "-expanded" suffix. Seven of them shipped
    // two backends each, a reflection-driven one and an "-expanded" one; only
    // the expanded half survives and the short name now means it. The other four
    // (lua, qt, qml, imgui) never had a thin twin and only wore the suffix
    // because their siblings did. The registry still answers to the old
    // spellings, but everything downstream of here keys off the lang STRING —
    // the output directory (bindings/<lang>), the per-language build recipe, the
    // wheel check — so an un-canonicalised alias would generate into
    // bindings/node and then look for bindings/node-expanded to build. Fold it
    // here, once.
    const auto canonical_lang = [](std::string lang) {
        static const char *kCollapsed[] = {"python", "nanobind", "node", "wasm", "julia", "csharp",
                                           "java",   "lua",      "qt",   "qml",  "imgui"};
        for (const char *base : kCollapsed) {
            if (lang == std::string(base) + "-expanded") {
                std::fprintf(stderr,
                             "rosetta_gen: target \"%s\" is deprecated — it is now spelled "
                             "\"%s\"\n",
                             lang.c_str(), base);
                return std::string(base);
            }
        }
        return lang;
    };

    // A target is either a bare string ("node") — using module_name — or an
    // object { "lang": ..., "name": ..., "link_options": [...] } overriding
    // the module name and optionally adding per-target linker flags.
    for (const auto &t : j.at("targets")) {
        TargetEntry e;
        if (t.is_string()) {
            e.lang = canonical_lang(t.get<std::string>());
            e.name = module_name;
        } else {
            e.lang = canonical_lang(t.at("lang").get<std::string>());
            e.name = t.contains("name") ? t.at("name").get<std::string>() : module_name;
            // Optional per-target linker flags (see TargetEntry::link_options).
            if (t.contains("link_options")) {
                for (const auto &o : t.at("link_options")) {
                    std::string flag = o.get<std::string>();
                    if (flag.empty()) {
                        throw std::runtime_error(
                            "\"link_options\" entries must not be empty");
                    }
                    e.link_options.push_back(std::move(flag));
                }
            }
            // Optional runtime pins (see TargetEntry::python and friends). A
            // bare version is left as written — the CMake side turns "3.11"
            // into python3.11 — so this only rejects what is certainly wrong.
            const auto str_field = [&t](const char *key) {
                std::string v;
                if (t.contains(key)) {
                    v = t.at(key).is_number() ? std::to_string(t.at(key).get<long long>())
                                              : t.at(key).get<std::string>();
                    if (v.empty()) {
                        throw std::runtime_error(std::string("a target's \"") + key +
                                                 "\" must not be empty");
                    }
                }
                return v;
            };
            e.python          = str_field("python");
            e.requires_python = str_field("requires_python");
            e.napi_version    = str_field("napi_version");
            e.node_engine     = str_field("node_engine");
            if (!e.napi_version.empty() &&
                e.napi_version.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("\"napi_version\" must be a number, got \"" +
                                         e.napi_version + "\"");
            }
            if (!e.requires_python.empty() &&
                e.requires_python.find_first_of("0123456789") == std::string::npos) {
                throw std::runtime_error("\"requires_python\" must name a version (e.g. \">=3.10\"), "
                                         "got \"" +
                                         e.requires_python + "\"");
            }
            // Optional per-target artifact destination (see TargetEntry::out_dir).
            if (t.contains("out_dir")) {
                const std::string d = t.at("out_dir").get<std::string>();
                if (d.empty()) {
                    throw std::runtime_error("a target's \"out_dir\" must not be empty");
                }
                e.out_dir = fs::weakly_canonical(base / fs::path(d)).string();
            }
            // Optional packaging, per target (see TargetEntry::wheel). Only the
            // two backends that emit a make_wheel.py can honour it, and a
            // manifest asking a markdown target for a wheel has made a mistake
            // worth naming rather than ignoring.
            const bool packages = e.lang == "python" || e.lang == "nanobind";
            for (const char *key : {"wheel", "wheel_dir"}) {
                if (t.contains(key) && !packages) {
                    throw std::runtime_error(
                        std::string("a \"") + e.lang + "\" target cannot take \"" + key +
                        "\": only the python and nanobind backends emit a make_wheel.py");
                }
            }
            if (t.contains("wheel")) {
                e.wheel = t.at("wheel").get<bool>();
            }
            if (t.contains("wheel_dir")) {
                const std::string d = t.at("wheel_dir").get<std::string>();
                if (d.empty()) {
                    throw std::runtime_error("a target's \"wheel_dir\" must not be empty");
                }
                e.wheel_dir = fs::weakly_canonical(base / fs::path(d)).string();
            }
        }
        m.targets.push_back(std::move(e));
    }

    // `out_dir` at the top level is the default for every target that names
    // none — one line when all the artifacts belong in the same place.
    if (j.contains("out_dir")) {
        const std::string d = j.at("out_dir").get<std::string>();
        if (d.empty()) {
            throw std::runtime_error("\"out_dir\" must not be empty");
        }
        m.out_dir = fs::weakly_canonical(base / fs::path(d)).string();
    }
    for (auto &t : m.targets) {
        if (t.out_dir.empty()) {
            t.out_dir = m.out_dir;
        }
    }

    // `wheel` / `wheel_dir` are PER-TARGET (see TargetEntry::wheel). They were
    // top-level once, because they began as defaults for --build's --wheel /
    // --wheel-dir and a flag applies to a whole run — but the two backends that
    // package already differ per target in the way that matters most to a wheel
    // (`python` picks the interpreter it is tagged for), and `out_dir`, the
    // field with the same shape, is per-target. A top-level key here would now
    // silently do nothing, which is the one outcome worth an error: a manifest
    // that used to ship wheels would quietly stop.
    for (const char *key : {"wheel", "wheel_dir"}) {
        if (j.contains(key)) {
            throw std::runtime_error(
                std::string("top-level \"") + key +
                "\" is no longer supported — move it onto the python / nanobind "
                "entries of \"targets\"");
        }
    }

    // Optional shared defaults, factoring the per-entry repetition out of
    // "classes" / "functions" / "extensions":
    //   "namespace"  — default C++ namespace for entry names that carry no
    //                  "::" of their own ("Serie" → "stressinv::Serie"). A
    //                  name containing "::" is taken verbatim (so fully
    //                  qualified spellings — incl. nested classes — keep
    //                  working), and a leading "::" pins an entry to the
    //                  global namespace ("::Thing" → "Thing").
    //   "header_dir" — directory fragment prepended to every entry header
    //                  ("Serie.h" → "stressinv/Serie.h").
    //
    // Both also appear on GROUP entries: an object inside "classes" /
    // "functions" carrying "entries" (a nested entry list) instead of being
    // an entry itself. A group's "namespace" appends to the inherited one
    // ("sinv" under "arch" → arch::sinv; a leading "::" makes it absolute
    // instead), its "header_dir" appends below the inherited dir, and an
    // optional group "header" is the default header for entries that spell
    // none — for a run of classes declared by one header. Groups nest, and
    // mix freely with plain entries in the same array.
    auto with_slash = [](std::string d) {
        if (!d.empty() && d.back() != '/') {
            d += '/';
        }
        return d;
    };
    auto qualify = [](const std::string &ns, const std::string &n) -> std::string {
        if (n.rfind("::", 0) == 0) {
            return n.substr(2); // explicit global namespace
        }
        if (!ns.empty() && n.find("::") == std::string::npos) {
            return ns + "::" + n;
        }
        return n;
    };

    // The defaults inherited by an entry: the composed namespace, the
    // composed ('/'-terminated) header dir, and the group's default header.
    struct EntryCtx {
        std::string ns;
        std::string dir;
        std::string header;
    };
    auto group_ctx = [&](const json &g, const EntryCtx &outer) {
        EntryCtx ctx = outer;
        if (g.contains("namespace")) {
            const std::string ns = g.at("namespace").get<std::string>();
            ctx.ns = (ns.rfind("::", 0) == 0) ? ns.substr(2)
                     : outer.ns.empty()       ? ns
                                              : outer.ns + "::" + ns;
        }
        if (g.contains("header_dir")) {
            ctx.dir = outer.dir + with_slash(g.at("header_dir").get<std::string>());
        }
        if (g.contains("header")) {
            ctx.header = g.at("header").get<std::string>();
        }
        return ctx;
    };
    auto entry_header = [](const json &e, const EntryCtx &ctx, const char *what) {
        const std::string h =
            e.contains("header") ? e.at("header").get<std::string>() : ctx.header;
        if (h.empty()) {
            throw std::runtime_error(std::string(what) +
                                     " entry has no \"header\" (and its group sets no default)");
        }
        return ctx.dir + h;
    };
    // An entry's optional "expose": the binding name, overriding the C++
    // identifier. It names a module attribute (and, for a class, a generated
    // trampoline), so it must be a plain identifier. Shared by classes, free
    // functions and extension methods.
    // An entry's optional "signature": the C++ function type of the ONE overload
    // to bind. Checked only for the shape the generated driver needs — a
    // parameter list with balanced parentheses and a return type before it — so
    // that a typo surfaces here, naming the manifest entry, instead of as a
    // template error deep in the driver. The types themselves are the compiler's
    // business; they are spliced verbatim.
    auto entry_signature = [](const json &e, const std::string &name, const char *what) {
        std::string sig;
        if (!e.contains("signature")) {
            return sig;
        }
        sig                     = e.at("signature").get<std::string>();
        const auto  open        = sig.find('(');
        int         depth       = 0;
        bool        balanced    = true;
        for (char ch : sig) {
            depth += ch == '(';
            depth -= ch == ')';
            balanced = balanced && depth >= 0;
        }
        if (open == std::string::npos || open == 0 || depth != 0 || !balanced ||
            sig.find(')') == std::string::npos) {
            throw std::runtime_error(
                std::string(what) + " \"" + name + "\": \"signature\" (\"" + sig +
                "\") must be a C++ function type, e.g. \"void(Mesh&, bool)\" — return type "
                "first, then a parenthesized parameter list");
        }
        return sig;
    };
    auto entry_expose = [](const json &e, const std::string &name, const char *what) {
        std::string expose;
        if (e.contains("expose")) {
            expose             = e.at("expose").get<std::string>();
            const bool ident   = !expose.empty() &&
                               (std::isalpha(static_cast<unsigned char>(expose[0])) ||
                                expose[0] == '_') &&
                               std::all_of(expose.begin(), expose.end(), [](char ch) {
                                   return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
                               });
            if (!ident) {
                throw std::runtime_error(std::string(what) + " \"" + name + "\": \"expose\" (\"" +
                                         expose + "\") must be a plain identifier");
            }
        }
        return expose;
    };

    const EntryCtx top{
        j.contains("namespace") ? j.at("namespace").get<std::string>() : std::string{},
        with_slash(j.contains("header_dir") ? j.at("header_dir").get<std::string>()
                                            : std::string{}),
        std::string{}};

    // A class entry whose "header" is a shell GLOB ("*.h", "solvers/**/*.h")
    // stands for every header it matches — one class entry per file, named
    // from the file's stem. The whole-folder shorthand for the common library
    // shape (one class per header): a manifest says which FOLDER to bind
    // instead of repeating a line per class, and adding a header to the tree
    // is enough to bind it. Optional, of course: nothing changes for a
    // manifest that spells its classes out.
    //
    // The pattern is resolved against each `user_include` root (not the
    // manifest dir like `user_sources`, since a class header is emitted into
    // `#include "..."` and so must be spelled relative to the include path),
    // with the composed `header_dir` in front. Only headers are taken
    // (.h/.hh/.hpp/.hxx), so "**/*" cannot drag a .cpp in.
    //
    // These entries are DEFERRED into `globbed` and appended after everything
    // spelled out, where they yield to it — see the flush below.
    std::vector<ClassEntry> globbed;
    auto expand_class_glob = [&](const json &c, const EntryCtx &ctx, const std::string &pattern) {
        // Per-class fields cannot mean anything for a whole folder: a name, a
        // binding rename, one class's annotation side-car, one class's
        // extension methods. Reject them rather than apply them N times.
        for (const char *key : {"name", "expose", "annotations", "extensions"}) {
            if (c.contains(key)) {
                throw std::runtime_error(std::string("class entry \"") + pattern +
                                         "\" is a glob (it stands for every header it matches), "
                                         "so it cannot carry \"" + key +
                                         "\" — spell that class out in its own entry");
            }
        }
        // "final" is uniform, so it does carry (a folder of leaf structs).
        const bool final_ = c.contains("final") && c.at("final").get<bool>();
        // Optional "exclude": glob patterns (same syntax, same roots) whose
        // matches are dropped — the headers in the folder that declare no
        // bound class, or a detail/ subtree. A single string is a one-element
        // list, as everywhere else.
        std::vector<std::string> exclude;
        if (c.contains("exclude")) {
            const auto &ex = c.at("exclude");
            if (ex.is_array()) {
                for (const auto &p : ex) {
                    exclude.push_back(ctx.dir + p.get<std::string>());
                }
            } else {
                exclude.push_back(ctx.dir + ex.get<std::string>());
            }
        }

        bool matched = false;
        for (const auto &inc : m.user_include) {
            std::vector<fs::path> dropped;
            for (const auto &p : exclude) {
                for (auto &d : expand_glob(inc, p)) {
                    dropped.push_back(std::move(d));
                }
            }
            for (const auto &p : expand_glob(inc, pattern)) {
                std::error_code ec;
                if (!fs::is_regular_file(p, ec)) {
                    continue;
                }
                const std::string ext = p.extension().string();
                if (ext != ".h" && ext != ".hh" && ext != ".hpp" && ext != ".hxx") {
                    continue;
                }
                if (std::find(dropped.begin(), dropped.end(), p) != dropped.end()) {
                    continue;
                }
                matched = true;
                // The derived name is the file's stem, so a header whose stem
                // is not a C++ identifier ("my-utils.h") has nothing to derive
                // from: skip it here, where the message names the file, rather
                // than emit a driver that will not compile.
                const std::string stem = p.stem().string();
                const bool ident = !stem.empty() &&
                                   (std::isalpha(static_cast<unsigned char>(stem[0])) ||
                                    stem[0] == '_') &&
                                   std::all_of(stem.begin(), stem.end(), [](char ch) {
                                       return std::isalnum(static_cast<unsigned char>(ch)) ||
                                              ch == '_';
                                   });
                if (!ident) {
                    std::fprintf(stderr,
                                 "rosetta_gen: warning: \"classes\" pattern \"%s\" matched %s, "
                                 "whose name cannot come from its stem — skipped (list it "
                                 "explicitly with a \"name\")\n",
                                 pattern.c_str(), p.filename().string().c_str());
                    continue;
                }
                ClassEntry e;
                e.header = fs::relative(p, inc, ec).generic_string();
                e.name   = qualify(ctx.ns, stem);
                e.final_ = final_;
                // Two include roots can hold the same relative header, and two
                // patterns can overlap; first match wins.
                if (std::none_of(globbed.begin(), globbed.end(), [&](const ClassEntry &g) {
                        return g.header == e.header || g.name == e.name;
                    })) {
                    globbed.push_back(std::move(e));
                }
            }
        }
        if (!matched) {
            std::fprintf(stderr,
                         "rosetta_gen: warning: \"classes\" pattern matched no headers: %s\n",
                         pattern.c_str());
        }
    };

    std::function<void(const json &, const EntryCtx &)> add_classes =
        [&](const json &arr, const EntryCtx &ctx) {
            for (const auto &c : arr) {
                // A group: local defaults plus a nested entry list.
                if (c.contains("entries")) {
                    if (c.contains("name")) {
                        throw std::runtime_error(
                            "a group (object with \"entries\") cannot carry \"name\"");
                    }
                    add_classes(c.at("entries"), group_ctx(c, ctx));
                    continue;
                }
                ClassEntry e;
                e.header = entry_header(c, ctx, "class");
                // A globbed header stands for a whole folder — see above.
                if (is_glob_pattern(e.header)) {
                    expand_class_glob(c, ctx, e.header);
                    continue;
                }
                // `name` is optional; fall back to the header's basename (stem).
                e.name = qualify(ctx.ns, c.contains("name") ? c.at("name").get<std::string>()
                                                            : fs::path(e.header).stem().string());
                // `expose` is optional: the binding name, overriding the C++
                // identifier (see ClassEntry::expose). Must be a plain
                // identifier — it names a module attribute and a trampoline.
                e.expose = entry_expose(c, e.name, "class");
                // `annotations` is optional: an out-of-line annotation JSON side-car
                // (doc/range/readonly/combobox keyed by member name). Baked into
                // bindings.h at generation time, so the user's header stays clean.
                if (c.contains("annotations")) {
                    e.annotations = fs::weakly_canonical(
                        base / fs::path(c.at("annotations").get<std::string>()));
                }
                // `final` is optional: suppress the trampoline (see ClassEntry::final_).
                if (c.contains("final")) {
                    e.final_ = c.at("final").get<bool>();
                }
                // `extensions` is optional: free functions (first parameter `Cls&`)
                // exposed as instance methods of this class — same entry shape as
                // "functions" (see ClassEntry::extensions).
                if (c.contains("extensions")) {
                    for (const auto &x : c.at("extensions")) {
                        FunctionEntry xe;
                        xe.name   = qualify(ctx.ns, x.at("name").get<std::string>());
                        xe.header = ctx.dir + x.at("header").get<std::string>();
                        xe.doc =
                            x.contains("doc") ? x.at("doc").get<std::string>() : std::string{};
                        // `expose` renames the method on the class it attaches to.
                        xe.expose = entry_expose(x, xe.name, "extension");
                        // Overload selection is a "functions" feature: an
                        // extension reaches the backends as a GenMethod, whose
                        // emitters spell ext_qualified in many more places than
                        // a free function's single address-of. Reject it here
                        // rather than bind the wrong overload silently.
                        if (x.contains("signature")) {
                            throw std::runtime_error(
                                "extension \"" + xe.name +
                                "\": \"signature\" (overload selection) is supported on "
                                "\"functions\" entries only — write the wrapper as a free "
                                "function, or give the extension a distinct name");
                        }
                        e.extensions.push_back(std::move(xe));
                    }
                }
                m.classes.push_back(std::move(e));
            }
        };
    if (j.contains("classes")) {
        add_classes(j.at("classes"), top);
    } else if (!preset.contains("classes")) {
        throw std::runtime_error("manifest has no \"classes\" (and no \"preset\" supplying any)");
    }
    if (preset.contains("classes")) {
        add_classes(preset.at("classes"), EntryCtx{}); // pristine: see the preset block
    }

    // Flush the glob-produced entries. They land AFTER everything spelled out
    // and yield to it, by NAME and by HEADER:
    //
    //   name   — the class is already bound, with whatever "expose" /
    //            "annotations" / "extensions" the explicit entry gave it. The
    //            glob simply has nothing to add.
    //   header — an entry that names a class of this header SPEAKS FOR THE
    //            HEADER: the glob leaves the file alone. That is what lets a
    //            header whose stem names nothing bindable ("types.h" declaring
    //            Tolerance and Mode) be listed class by class without the glob
    //            also inventing a class called `types`.
    //
    // The second rule has a cost worth naming out loud rather than leaving to
    // be discovered: if the header declares BOTH a class named after the file
    // and another one listed explicitly, taking the file over drops the first.
    // Only the manifest's author can say which was meant — the loader never
    // reads a bound header — so it is a note on stderr and a one-line fix
    // (list the stem class too).
    for (auto &g : globbed) {
        const auto same_name = std::find_if(m.classes.begin(), m.classes.end(),
                                            [&](const ClassEntry &e) { return e.name == g.name; });
        if (same_name != m.classes.end()) {
            continue;
        }
        const auto same_header = std::find_if(
            m.classes.begin(), m.classes.end(),
            [&](const ClassEntry &e) { return e.header == g.header; });
        if (same_header != m.classes.end()) {
            std::fprintf(stderr,
                         "rosetta_gen: note: \"%s\" is listed explicitly (class \"%s\"), so the "
                         "pattern that also matched it contributes nothing there — class \"%s\" "
                         "is NOT bound. List it too if the header declares both; name the header "
                         "in the pattern's \"exclude\" if it does not (silences this)\n",
                         g.header.c_str(), same_header->name.c_str(), g.name.c_str());
            continue;
        }
        m.classes.push_back(std::move(g));
    }

    // `functions` is optional: free (non-member) functions to bind. Each entry
    // gives the (optionally qualified) function name and its declaring header;
    // `doc` is an optional description (free functions carry no in-source
    // annotation, keeping the user's headers untouched). Same group support
    // as "classes" — a shared-header group reads especially well here.
    if (j.contains("functions") || preset.contains("functions")) {
        std::function<void(const json &, const EntryCtx &)> add_functions =
            [&](const json &arr, const EntryCtx &ctx) {
                for (const auto &f : arr) {
                    if (f.contains("entries")) {
                        if (f.contains("name")) {
                            throw std::runtime_error(
                                "a group (object with \"entries\") cannot carry \"name\"");
                        }
                        add_functions(f.at("entries"), group_ctx(f, ctx));
                        continue;
                    }
                    FunctionEntry e;
                    e.name   = qualify(ctx.ns, f.at("name").get<std::string>());
                    e.header = entry_header(f, ctx, "function");
                    e.doc    = f.contains("doc") ? f.at("doc").get<std::string>() : std::string{};
                    // `expose` is optional: the binding name, overriding the C++
                    // identifier (see FunctionEntry::expose).
                    e.expose = entry_expose(f, e.name, "function");
                    // `signature` is optional: bind ONE overload of an
                    // overloaded name (see FunctionEntry::signature).
                    e.signature = entry_signature(f, e.name, "function");
                    m.functions.push_back(std::move(e));
                }
            };
        if (j.contains("functions")) {
            add_functions(j.at("functions"), top);
        }
        if (preset.contains("functions")) {
            add_functions(preset.at("functions"), EntryCtx{}); // pristine: see the preset block
        }
    }

    // `sequences` is optional: foreign sequence containers. A string entry is a
    // qualified TEMPLATE name with one type parameter ("GEO::vector"); an object
    // entry { "type": "Eigen::VectorXd" } registers a CONCRETE type, spelled
    // exactly as written (see SequenceEntry). The two cannot be told apart by
    // looking at the text — "Eigen::VectorXd" is a perfectly good template name
    // — and picking wrong emits a specialization that does not compile, so the
    // manifest says which it is.
    // `matrices` is the same list one dimension up (rosetta::is_matrix), with
    // the same two entry forms — hence one reader for both.
    const auto read_containers = [](const json &arr, const char *field) {
        std::vector<ContainerEntry> out;
        for (const auto &s : arr) {
            ContainerEntry e;
            if (s.is_object()) {
                if (s.contains("type") == s.contains("template")) {
                    throw std::runtime_error(std::string("a \"") + field +
                                             "\" object entry needs exactly one of \"type\" "
                                             "(a concrete type) or \"template\" (a "
                                             "one-parameter template)");
                }
                e.exact = s.contains("type");
                e.name  = s.at(e.exact ? "type" : "template").get<std::string>();
            } else {
                e.name = s.get<std::string>();
            }
            if (e.name.empty()) {
                throw std::runtime_error(std::string("\"") + field +
                                         "\" entries must not be empty");
            }
            out.push_back(std::move(e));
        }
        return out;
    };
    if (j.contains("sequences")) {
        m.sequences = read_containers(j.at("sequences"), "sequences");
    }
    if (j.contains("matrices")) {
        m.matrices = read_containers(j.at("matrices"), "matrices");
    }

    // `module_init` is optional: C++ statements the generated module runs when
    // it LOADS, plus the headers that declare them. Two forms — a bare array of
    // statements, or an object with "headers" + "statements" — because the
    // headers are usually needed (an init statement tends to name a namespace
    // the bound classes never mention) but not always.
    if (j.contains("module_init")) {
        const json &mi = j.at("module_init");
        if (mi.is_array()) {
            m.module_init.statements = mi.get<std::vector<std::string>>();
        } else if (mi.is_object()) {
            if (mi.contains("headers")) {
                // Spelled exactly as written — these are include paths for the
                // emitted `#include`, not entry headers, so the manifest's
                // "header_dir" default does not apply to them.
                m.module_init.headers = mi.at("headers").get<std::vector<std::string>>();
            }
            if (!mi.contains("statements")) {
                throw std::runtime_error("\"module_init\" object needs \"statements\"");
            }
            m.module_init.statements = mi.at("statements").get<std::vector<std::string>>();
        } else {
            throw std::runtime_error(
                "\"module_init\" must be an array of statements or an object with "
                "\"headers\" + \"statements\"");
        }
        for (const std::string &st : m.module_init.statements) {
            if (st.empty()) {
                throw std::runtime_error("\"module_init\" statements must not be empty");
            }
        }
    }

    // `out_params` is optional: which parameters a method RETURNS through a
    // reference. Not inferable — `assign_points(vector<double>&, ...)` takes its
    // vector as an input it may steal from and `get_doubles(..., vector<double>&,
    // index_t&)` fills its own as an output, and the two are the same C++.
    if (j.contains("out_params")) {
        for (const auto &[key, idx] : j.at("out_params").items()) {
            if (key.empty()) {
                throw std::runtime_error("\"out_params\" keys must name a method or function");
            }
            std::vector<std::size_t> indices;
            for (const auto &i : idx) {
                indices.push_back(i.get<std::size_t>());
            }
            if (indices.empty()) {
                throw std::runtime_error("\"out_params\" entry \"" + key +
                                         "\" lists no parameter index");
            }
            m.out_params[key] = std::move(indices);
        }
    }

    // `generated_headers` is optional: headers the bound library's own build
    // system would have generated. Either a "template" file with @KEY@
    // placeholders plus "substitutions", or literal "content" lines — resolved
    // to finished text HERE, so the driver carries no file paths and a missing
    // template is reported against the manifest that named it.
    if (j.contains("generated_headers")) {
        for (const auto &g : j.at("generated_headers")) {
            GeneratedHeaderEntry e;
            e.path = g.at("path").get<std::string>();
            if (e.path.empty() || fs::path(e.path).is_absolute()) {
                throw std::runtime_error("\"generated_headers\" path must be a relative include "
                                         "path, e.g. \"geogram/version.h\"");
            }
            if (g.contains("template") == g.contains("content")) {
                throw std::runtime_error(
                    "\"generated_headers\" entry \"" + e.path +
                    "\" needs exactly one of \"template\" (a file with @KEY@ placeholders) "
                    "or \"content\" (literal lines)");
            }
            if (g.contains("content")) {
                for (const auto &line : g.at("content")) {
                    e.content += line.get<std::string>() + "\n";
                }
            } else {
                const fs::path tpl =
                    fs::weakly_canonical(base / g.at("template").get<std::string>());
                std::ifstream in(tpl);
                if (!in) {
                    throw std::runtime_error("\"generated_headers\": cannot read template '" +
                                             tpl.string() + "'");
                }
                std::stringstream ss;
                ss << in.rdbuf();
                e.content = ss.str();
                // configure_file's @KEY@ form, and only that form: ${KEY} would
                // collide with the shell/CMake expansions such templates carry.
                if (g.contains("substitutions")) {
                    for (const auto &[k, v] : g.at("substitutions").items()) {
                        const std::string tok = "@" + k + "@";
                        const std::string rep = v.get<std::string>();
                        for (std::size_t pos = e.content.find(tok); pos != std::string::npos;
                             pos             = e.content.find(tok, pos + rep.size())) {
                            e.content.replace(pos, tok.size(), rep);
                        }
                    }
                }
                // A leftover placeholder means the substitution list is
                // incomplete: the emitted header would carry a literal @KEY@
                // into the compile. Name the first one.
                const auto at = e.content.find('@');
                if (at != std::string::npos) {
                    const auto end = e.content.find('@', at + 1);
                    if (end != std::string::npos) {
                        throw std::runtime_error(
                            "\"generated_headers\" entry \"" + e.path +
                            "\": no substitution for " + e.content.substr(at, end - at + 1));
                    }
                }
            }
            m.generated_headers.push_back(std::move(e));
        }
    }

    // `interop` is optional: foreign libraries whose types the target's binding
    // framework marshals natively (see Manifest::interop). Validated against
    // the names rosetta knows, because a typo here is silent otherwise — the
    // trait would simply never be enabled and every Eigen-typed member would
    // keep being skipped, with nothing to point at.
    if (j.contains("interop")) {
        for (const auto &s : j.at("interop")) {
            std::string name = s.get<std::string>();
            if (name != "eigen") {
                throw std::runtime_error("unknown \"interop\" entry \"" + name +
                                         "\" — known: eigen");
            }
            if (std::find(m.interop.begin(), m.interop.end(), name) == m.interop.end()) {
                m.interop.push_back(std::move(name));
            }
        }
    }

    // `plugins` is optional: extra .cpp sources (e.g. a custom Backend +
    // BackendRegistrar) compiled into the driver. Resolved to absolute paths.
    if (j.contains("plugins")) {
        for (const auto &p : j.at("plugins")) {
            m.plugins.push_back(
                fs::weakly_canonical(base / fs::path(p.get<std::string>())).string());
        }
    }

    // `user_sources` is optional: a list of user .cpp files compiled directly
    // into every generated binding target (use when the bound headers only
    // declare the API and you want the bodies built in rather than linked from a
    // pre-built user_lib). A single string is accepted as a one-element list.
    // Each entry is resolved relative to the manifest (or taken absolute) and may
    // be a shell glob ("src/algorithms/*.cpp"; "src/**/*.cxx" recurses —
    // see expand_glob), expanded here at generation time. An entry starting
    // with '!' is an EXCLUSION (same glob syntax): after all inclusions
    // expand, the files it matches are dropped — so a near-complete tree is
    // one recursive glob plus the odd carve-out:
    //   ["src/**/*.cxx", "!src/mesh_imp/volume_mesh/tetgenMesh.cxx"]
    // Exclusions are order-independent (they apply to the final list).
    if (j.contains("user_sources")) {
        const auto &us = j.at("user_sources");
        std::vector<std::string> excluded;
        auto add = [&](const std::string &entry) {
            const bool  excl = !entry.empty() && entry[0] == '!';
            const std::string p = excl ? entry.substr(1) : entry;
            auto       &dst  = excl ? excluded : m.user_sources;
            if (is_glob_pattern(p)) {
                std::vector<fs::path> matches = expand_glob(base, p);
                if (matches.empty()) {
                    std::fprintf(stderr,
                                 "rosetta_gen: warning: \"user_sources\" %s matched "
                                 "no files: %s\n",
                                 excl ? "exclusion" : "pattern", p.c_str());
                }
                for (const auto &mt : matches) {
                    dst.push_back(mt.string());
                }
            } else {
                dst.push_back(fs::weakly_canonical(base / fs::path(p)).string());
            }
        };
        if (us.is_array()) {
            for (const auto &e : us) {
                add(e.get<std::string>());
            }
        } else {
            add(us.get<std::string>());
        }
        // Apply the '!' exclusions, then drop duplicates (a file named
        // literally and also matched by a glob, or overlapping globs) —
        // keeping first-seen order — so a target never lists the same source
        // twice (which CMake rejects).
        std::vector<std::string> deduped;
        for (const auto &src : m.user_sources) {
            if (std::find(excluded.begin(), excluded.end(), src) != excluded.end()) {
                continue;
            }
            if (std::find(deduped.begin(), deduped.end(), src) == deduped.end()) {
                deduped.push_back(src);
            }
        }
        m.user_sources = std::move(deduped);
    }

    // `compile_definitions` is optional: preprocessor definitions ("NAME" or
    // "NAME=VALUE") applied to the driver and to every compiled binding target,
    // so they reach the bound headers and the user_sources alike (e.g. a
    // third-party lib's configuration switches: XXX_USE_BUILTIN_DEPS,
    // XXX_WITH_HLBFGS). A single string is accepted as a one-element list.
    if (j.contains("compile_definitions")) {
        const auto &cd  = j.at("compile_definitions");
        auto        add = [&](const std::string &d) {
            if (d.empty()) {
                throw std::runtime_error("\"compile_definitions\" entries must not be empty");
            }
            m.compile_definitions.push_back(d);
        };
        if (cd.is_array()) {
            for (const auto &e : cd) {
                add(e.get<std::string>());
            }
        } else {
            add(cd.get<std::string>());
        }
    }

    // `cpp26_root` is optional: the path to the C++26 / P2996 reflection
    // toolchain root (the clang-p2996 build dir, holding bin/clang++ and lib/).
    // Stored verbatim so a value like "$ENV{HOME}/..." or an absolute path is
    // baked straight into the generated CMakeLists. Only `rest`, the one
    // backend whose generated code still splices reflections, needs it; every
    // other target builds with a stock compiler.
    if (j.contains("cpp26_root")) {
        m.cpp26_root = j.at("cpp26_root").get<std::string>();
    }
    // Optional finer-grained overrides; each defaults from cpp26_root if unset.
    //   cpp26_cxx — C++ compiler (name or path)   cpp26_cc — C compiler
    //   cpp26_lib — fork stdlib dir (libc++/libc++abi) for -L / -rpath
    if (j.contains("cpp26_cxx")) {
        m.cpp26_cxx = j.at("cpp26_cxx").get<std::string>();
    }
    if (j.contains("cpp26_cc")) {
        m.cpp26_cc = j.at("cpp26_cc").get<std::string>();
    }
    if (j.contains("cpp26_lib")) {
        m.cpp26_lib = j.at("cpp26_lib").get<std::string>();
    }
    // Optional Qt 6 install prefix for the qt / qml backends.
    if (j.contains("qt_dir")) {
        m.qt_dir = j.at("qt_dir").get<std::string>();
    }

    // `build_type` is optional: the default CMAKE_BUILD_TYPE baked into every
    // compiled backend's generated CMakeLists. Case-insensitive on input,
    // stored in CMake's canonical spelling. Emitted inside
    // if(NOT CMAKE_BUILD_TYPE), so -DCMAKE_BUILD_TYPE=... at configure time
    // still wins.
    if (j.contains("build_type")) {
        std::string bt = j.at("build_type").get<std::string>();
        std::string lo = bt;
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        static const std::pair<const char *, const char *> kBuildTypes[] = {
            {"debug", "Debug"},
            {"release", "Release"},
            {"relwithdebinfo", "RelWithDebInfo"},
            {"minsizerel", "MinSizeRel"}};
        for (const auto &[lower, canon] : kBuildTypes) {
            if (lo == lower) {
                m.build_type = canon;
                break;
            }
        }
        if (m.build_type.empty()) {
            throw std::runtime_error("build_type must be \"Debug\", \"Release\", "
                                     "\"RelWithDebInfo\" or \"MinSizeRel\" (got \"" +
                                     bt + "\")");
        }
    }
    // `optimization` is optional: an explicit optimization level applied to
    // every compiled backend after the build type's own flags — so this -O
    // wins over the level the build type implies. The leading "-" may be
    // omitted ("O2" ⇒ "-O2").
    if (j.contains("optimization")) {
        std::string o = j.at("optimization").get<std::string>();
        if (!o.empty() && o[0] != '-') {
            o = "-" + o;
        }
        static const char *kLevels[] = {"-O0", "-O1", "-O2", "-O3",
                                        "-Os", "-Oz", "-Og", "-Ofast"};
        if (std::find_if(std::begin(kLevels), std::end(kLevels),
                         [&](const char *l) { return o == l; }) == std::end(kLevels)) {
            throw std::runtime_error(
                "optimization must be one of -O0, -O1, -O2, -O3, -Os, -Oz, -Og, "
                "-Ofast (got \"" + j.at("optimization").get<std::string>() + "\")");
        }
        m.optimization = o;
    }
    // `cxx_standard` is optional: the C++ standard the user_sources compile
    // with, emitted as a per-source -std that wins over the target's own
    // standard for those files only (the generated binding TU keeps its
    // backend's standard, which its runtime headers require). A number or a
    // string is accepted ("17" or 17).
    if (j.contains("cxx_standard")) {
        const auto &cs = j.at("cxx_standard");
        std::string std_str =
            cs.is_number() ? std::to_string(cs.get<int>()) : cs.get<std::string>();
        static const char *kStandards[] = {"11", "14", "17", "20", "23", "26"};
        if (std::find_if(std::begin(kStandards), std::end(kStandards),
                         [&](const char *s) { return std_str == s; }) ==
            std::end(kStandards)) {
            throw std::runtime_error(
                "cxx_standard must be one of 11, 14, 17, 20, 23, 26 (got \"" +
                std_str + "\")");
        }
        m.cxx_standard = std_str;
    }

    // `version` is optional: the distribution version stamped into the
    // packaging artifacts — the pyproject.toml the python / nanobind
    // backends emit for wheel builds. Nothing else consumes
    // it, so a manifest that never packages can leave it out (the backends
    // then default to 0.1.0). A number is accepted and stringified so
    // "version": 2 doesn't have to be quoted. Validated loosely against
    // PEP 440's shape rather than its full grammar: pip is the authority, we
    // only catch the spellings that are obviously not versions (empty, leading
    // "v", embedded spaces).
    if (j.contains("version")) {
        const auto &v = j.at("version");
        std::string ver =
            v.is_number() ? (v.is_number_integer() ? std::to_string(v.get<long long>())
                                                   : std::to_string(v.get<double>()))
                          : v.get<std::string>();
        const bool ok =
            !ver.empty() && std::isdigit(static_cast<unsigned char>(ver.front())) &&
            std::all_of(ver.begin(), ver.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '+' || ch == '!';
            });
        if (!ok) {
            throw std::runtime_error(
                "version must be a PEP 440 release string starting with a digit, e.g. "
                "\"1.2.0\" or \"0.3.0rc1\" (got \"" + ver + "\")");
        }
        m.version = std::move(ver);
    }

    // `user_lib` is optional: the external libraries the generated bindings link
    // against. Use it when the bound headers only *declare* the API and its
    // bodies are compiled into separate shared/static libraries. `name` is the
    // library base name (-l<name>); `dir` is the directory holding it, resolved
    // to an absolute path (relative to the manifest) for -L / rpath.
    //
    // It is either ONE object or an ARRAY of them — the bound library plus the
    // pre-built libraries it depends on. Array order is the link order, so list
    // dependents before their dependencies when linking static archives.
    if (j.contains("user_lib")) {
        const auto &ul = j.at("user_lib");
        if (!ul.is_object() && !ul.is_array()) {
            throw std::runtime_error(
                "\"user_lib\" must be an object {name, dir, link} or an array of them");
        }
        if (ul.is_array() && ul.empty()) {
            throw std::runtime_error("\"user_lib\" array must not be empty");
        }
        const auto parse_one = [&](const nlohmann::json &e) {
            if (!e.is_object()) {
                throw std::runtime_error("each \"user_lib\" entry must be an object {name, dir, link}");
            }
            if (!e.contains("name") || !e.contains("dir")) {
                throw std::runtime_error(
                    "each \"user_lib\" entry needs a \"name\" and a \"dir\"");
            }
            UserLibEntry u;
            u.name = e.at("name").get<std::string>();
            if (u.name.empty()) {
                throw std::runtime_error("\"user_lib\" entry has an empty \"name\"");
            }
            u.dir = fs::weakly_canonical(base / fs::path(e.at("dir").get<std::string>())).string();
            // Optional "link": prefer linking the shared or the static form of the
            // library. "dynamic" is accepted as an alias for "shared". Default shared.
            // (WebAssembly always links static regardless — see the wasm backend.)
            if (e.contains("link")) {
                std::string link = e.at("link").get<std::string>();
                if (link == "dynamic") {
                    link = "shared";
                }
                if (link != "shared" && link != "static") {
                    throw std::runtime_error(
                        "user_lib.link must be \"shared\", \"dynamic\", or \"static\" (got \"" +
                        link + "\")");
                }
                u.link = link;
            }
            m.user_libs.push_back(std::move(u));
        };
        if (ul.is_object()) {
            parse_one(ul);
        } else {
            for (const auto &e : ul) {
                parse_one(e);
            }
        }
    }

    if (m.generator_name.empty()) {
        throw std::runtime_error(
            "cannot derive generator_name (set it explicitly in the manifest)");
    }
    if (m.targets.empty()) {
        throw std::runtime_error("manifest has no targets");
    }
    if (m.classes.empty()) {
        throw std::runtime_error("manifest has no class entries");
    }

    // Every class and every free function binds under ONE module-level name:
    // its "expose" override, or its unqualified C++ identifier. Two entries
    // resolving to the same name would collide in the generated module (and
    // two classes would additionally collide in C++, their trampolines both
    // named Py_<name> / Js_<name>) — catch it here, where the fix ("expose"
    // one of them) is a one-line manifest edit. Classes and functions share
    // the module namespace, so the check spans both.
    {
        auto exposed = [](const std::string &name, const std::string &expose) {
            if (!expose.empty()) {
                return expose;
            }
            const auto pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        };
        struct Bound {
            std::string name;   // C++ spelling, for the message
            std::string as;     // module-level name it binds under
            const char *what;   // "class" / "function"
        };
        std::vector<Bound> bound;
        bound.reserve(m.classes.size() + m.functions.size());
        for (const auto &c : m.classes) {
            bound.push_back({c.name, exposed(c.name, c.expose), "class"});
        }
        for (const auto &f : m.functions) {
            bound.push_back({f.name, exposed(f.name, f.expose), "function"});
        }
        for (std::size_t i = 0; i < bound.size(); ++i) {
            for (std::size_t j = i + 1; j < bound.size(); ++j) {
                if (bound[i].as == bound[j].as) {
                    throw std::runtime_error(std::string(bound[i].what) + " \"" + bound[i].name +
                                             "\" and " + bound[j].what + " \"" + bound[j].name +
                                             "\" both bind as \"" + bound[i].as +
                                             "\" — rename one with \"expose\"");
                }
            }
        }
    }

    // "doc_comments": read the documentation the bound headers already carry.
    // Off is spelled explicitly; anything else (including absent) leaves it on,
    // because a library's own comments are what a generated binding should say
    // and re-reading files rosetta_gen has already located costs nothing.
    if (j.contains("doc_comments")) {
        if (!j.at("doc_comments").is_boolean()) {
            throw std::runtime_error("\"doc_comments\" must be true or false");
        }
        m.doc_comments = j.at("doc_comments").get<bool>();
    }
    if (m.doc_comments) {
        // Every header a bound entity is declared in — classes, their extension
        // methods, and free functions. Each is an INCLUDE path ("geom/point.h"),
        // so it is resolved against the include roots, first match wins, exactly
        // as the compiler will resolve it later.
        std::vector<std::string> headers;
        auto want = [&headers](const std::string &h) {
            if (!h.empty() && std::find(headers.begin(), headers.end(), h) == headers.end()) {
                headers.push_back(h);
            }
        };
        for (const auto &c : m.classes) {
            want(c.header);
            for (const auto &x : c.extensions) {
                want(x.header);
            }
        }
        for (const auto &f : m.functions) {
            want(f.header);
        }
        for (const std::string &h : headers) {
            for (const auto &inc : m.user_include) {
                std::error_code ec;
                const fs::path  p = inc / h;
                if (!fs::is_regular_file(p, ec)) {
                    continue;
                }
                merge_doc_maps(m.harvested_docs, harvest_doc_comments_file(p));
                break;
            }
            // A header that resolves nowhere is NOT an error here: it may be a
            // "generated_headers" entry that does not exist yet, or live on an
            // include path only the compiler knows. It simply contributes no
            // documentation.
        }
    }

    // Extension methods bind as members of their class, so the same rule
    // applies per class rather than module-wide.
    for (const auto &c : m.classes) {
        for (std::size_t i = 0; i < c.extensions.size(); ++i) {
            for (std::size_t j = i + 1; j < c.extensions.size(); ++j) {
                auto exposed = [](const FunctionEntry &e) {
                    if (!e.expose.empty()) {
                        return e.expose;
                    }
                    const auto pos = e.name.rfind("::");
                    return pos == std::string::npos ? e.name : e.name.substr(pos + 2);
                };
                if (exposed(c.extensions[i]) == exposed(c.extensions[j])) {
                    throw std::runtime_error(
                        "extensions \"" + c.extensions[i].name + "\" and \"" +
                        c.extensions[j].name + "\" of class \"" + c.name + "\" both bind as \"" +
                        exposed(c.extensions[i]) + "\" — rename one with \"expose\"");
                }
            }
        }
    }

    return m;
}
