// Copyright (c) fmaerten@gmail.com
// License: MIT

// The doc-comment harvester — see doccomments.h for what it is for and what it
// deliberately is not. Three layers, bottom up:
//
//   1. lexical helpers   — identifiers, literals, trimming, balanced skipping;
//   2. Doxygen parsing   — a comment block -> brief / detail / @param / @return;
//   3. the scanner       — a walk over the header that tracks scopes and reads
//                          declarations, attaching each pending comment to the
//                          declaration that follows it.

#include "doccomments.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

    // ---- 1. lexical helpers ------------------------------------------------

    bool is_ident_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }
    bool is_ident_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }
    bool is_space(char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }

    std::string trim(const std::string &s) {
        std::size_t b = 0, e = s.size();
        while (b < e && is_space(s[b])) {
            ++b;
        }
        while (e > b && is_space(s[e - 1])) {
            --e;
        }
        return s.substr(b, e - b);
    }

    // Collapse the runs of whitespace a wrapped declaration is full of, so
    // "const std::string\n        &tag" compares and reads as one line.
    std::string squeeze(const std::string &s) {
        std::string out;
        bool        sp = false;
        for (char c : s) {
            if (is_space(c)) {
                sp = true;
                continue;
            }
            if (sp && !out.empty()) {
                out += ' ';
            }
            sp = false;
            out += c;
        }
        return out;
    }

    // The C++ keywords that can end a type and so must never be mistaken for a
    // parameter's or member's name ("unsigned", "int", "const" ...).
    bool is_keyword(const std::string &w) {
        static const char *kw[] = {
            "alignas",   "alignof",  "auto",     "bool",     "char",      "char8_t",
            "char16_t",  "char32_t", "class",    "const",    "consteval", "constexpr",
            "constinit", "decltype", "double",   "enum",     "explicit",  "extern",
            "float",     "friend",   "inline",   "int",      "long",      "mutable",
            "noexcept",  "operator", "register", "requires", "short",     "signed",
            "sizeof",    "static",   "struct",   "template", "this",      "thread_local",
            "typedef",   "typename", "union",    "unsigned", "using",     "virtual",
            "void",      "volatile", "wchar_t",  "return",   "new",       "delete",
            "throw",     "if",       "for",      "while",    "switch",    "catch",
            "do",        "else",
        };
        for (const char *k : kw) {
            if (w == k) {
                return true;
            }
        }
        return false;
    }

    // Words that INTRODUCE a type rather than qualify a declarator. If all that
    // precedes a trailing identifier is these, the identifier is the type
    // ("const Mode", "enum Mode") and the parameter has no name.
    bool is_type_introducer(const std::string &w) {
        return w == "const" || w == "volatile" || w == "enum" || w == "struct" || w == "class" ||
               w == "union" || w == "typename";
    }

    // The last identifier of `s`, provided it ENDS the string. Returns "" when
    // the text ends in anything else — "std::vector<Foo>" ends with '>', so its
    // trailing "Foo" is part of the type, not a declarator name.
    std::string trailing_identifier(const std::string &s, std::size_t *start_out = nullptr) {
        std::size_t e = s.size();
        while (e > 0 && is_space(s[e - 1])) {
            --e;
        }
        if (e == 0 || !is_ident_char(s[e - 1])) {
            return {};
        }
        std::size_t b = e;
        while (b > 0 && is_ident_char(s[b - 1])) {
            --b;
        }
        if (!is_ident_start(s[b])) {
            return {};
        }
        if (start_out) {
            *start_out = b;
        }
        return s.substr(b, e - b);
    }

    // ---- literal / comment skipping ---------------------------------------

    // Advance past a string or character literal starting at s[i] (which is the
    // opening quote). Handles raw strings, whose delimiter makes escapes inert.
    void skip_literal(const std::string &s, std::size_t &i) {
        // A raw string is introduced by R"delim( ... )delim". The R and any
        // encoding prefix sit before the quote; the caller only knows about the
        // quote, so look back one character for the R.
        if (s[i] == '"' && i > 0 && s[i - 1] == 'R') {
            const std::size_t open = s.find('(', i + 1);
            if (open != std::string::npos) {
                const std::string delim = s.substr(i + 1, open - i - 1);
                const std::string close = ")" + delim + "\"";
                const std::size_t end   = s.find(close, open + 1);
                i                       = end == std::string::npos ? s.size() : end + close.size();
                return;
            }
        }
        const char quote = s[i++];
        while (i < s.size()) {
            if (s[i] == '\\') {
                i += 2;
                continue;
            }
            if (s[i] == quote) {
                ++i;
                return;
            }
            ++i;
        }
    }

    // Advance past a preprocessor directive, including backslash continuations.
    void skip_pp_line(const std::string &s, std::size_t &i) {
        while (i < s.size()) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                // A line splice: step over the newline it hides.
                std::size_t j = i + 1;
                while (j < s.size() && (s[j] == '\r' || s[j] == ' ' || s[j] == '\t')) {
                    ++j;
                }
                if (j < s.size() && s[j] == '\n') {
                    i = j + 1;
                    continue;
                }
            }
            if (s[i] == '\n') {
                return;
            }
            ++i;
        }
    }

    // ---- 2. Doxygen parsing ------------------------------------------------

    // Strip the comment markers from one raw block, leaving its text lines.
    // Handles both the `///` / `//!` run and the `/** ... */` form (whose
    // continuation lines conventionally start with a decorative '*').
    std::vector<std::string> comment_lines(const std::string &raw) {
        std::vector<std::string> out;
        std::istringstream       in(raw);
        std::string              line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            // Leading decoration of a block comment's continuation lines.
            if (!t.empty() && t[0] == '*' && (t.size() == 1 || t[1] != '/')) {
                t = trim(t.substr(1));
            }
            out.push_back(t);
        }
        // Drop blank lines at both ends; interior ones separate paragraphs.
        while (!out.empty() && out.front().empty()) {
            out.erase(out.begin());
        }
        while (!out.empty() && out.back().empty()) {
            out.pop_back();
        }
        return out;
    }

    // A Doxygen command at the start of a line: "@param", "\return", ... The
    // returned name excludes the sigil; `rest` is what follows it on the line.
    bool line_command(const std::string &line, std::string &cmd, std::string &rest) {
        if (line.size() < 2 || (line[0] != '@' && line[0] != '\\')) {
            return false;
        }
        std::size_t i = 1;
        while (i < line.size() && is_ident_char(line[i])) {
            ++i;
        }
        if (i == 1) {
            return false;
        }
        cmd  = line.substr(1, i - 1);
        rest = trim(line.substr(i));
        return true;
    }

    // Doxygen's direction markers on a @param: "@param[in,out] name text".
    void strip_param_direction(std::string &rest) {
        if (!rest.empty() && rest[0] == '[') {
            const std::size_t close = rest.find(']');
            if (close != std::string::npos) {
                rest = trim(rest.substr(close + 1));
            }
        }
    }

    // Commands whose text is not documentation of THIS entity, and which are
    // therefore dropped rather than rendered (grouping, file-level bookkeeping,
    // template parameters the binding never sees).
    bool is_ignored_command(const std::string &cmd) {
        static const char *ig[] = {
            "file",     "brief",       "details", "author",   "date",    "version",   "ingroup",
            "defgroup", "addtogroup",  "name",    "internal", "copydoc", "copybrief", "tparam",
            "relates",  "relatesalso", "{",       "}",        "class",   "struct",    "namespace",
            "fn",       "var",         "typedef", "enum",     "def",     "headerfile"};
        for (const char *k : ig) {
            if (cmd == k) {
                return true;
            }
        }
        return false;
    }

    // Commands rendered as a labelled paragraph in the docstring.
    const char *command_label(const std::string &cmd) {
        if (cmd == "note") {
            return "Note:";
        }
        if (cmd == "warning" || cmd == "attention") {
            return "Warning:";
        }
        if (cmd == "deprecated") {
            return "Deprecated:";
        }
        if (cmd == "throws" || cmd == "throw" || cmd == "exception") {
            return "Throws:";
        }
        if (cmd == "pre") {
            return "Precondition:";
        }
        if (cmd == "post") {
            return "Postcondition:";
        }
        if (cmd == "see" || cmd == "sa") {
            return "See also:";
        }
        if (cmd == "since") {
            return "Since:";
        }
        return nullptr;
    }

    struct ParsedComment {
        std::string                                      text;    // brief + detail + labelled notes
        std::string                                      returns; // @return
        std::vector<std::pair<std::string, std::string>> params;  // (name, text), in order
    };

    ParsedComment parse_doxygen(const std::string &raw) {
        ParsedComment                  out;
        const std::vector<std::string> lines = comment_lines(raw);
        std::vector<std::string>       body;  // untagged prose
        std::vector<std::string>       notes; // labelled paragraphs

        // Where continuation lines of the current command go. Doxygen lets a
        // command's text run until the next command or a blank line, and real
        // headers rely on it heavily for long @param descriptions.
        enum class Sink { Body, Returns, Param, Note, Drop } sink = Sink::Body;

        auto append = [](std::string &dst, const std::string &add) {
            if (add.empty()) {
                return;
            }
            if (!dst.empty()) {
                dst += ' ';
            }
            dst += add;
        };

        for (const std::string &line : lines) {
            std::string cmd, rest;
            if (line_command(line, cmd, rest)) {
                if (cmd == "param") {
                    strip_param_direction(rest);
                    // "@param name the rest of the text"
                    std::size_t sp = 0;
                    while (sp < rest.size() && !is_space(rest[sp])) {
                        ++sp;
                    }
                    const std::string pname = rest.substr(0, sp);
                    // A @param without a name documents nothing findable.
                    if (pname.empty()) {
                        sink = Sink::Drop;
                        continue;
                    }
                    out.params.emplace_back(pname, trim(rest.substr(sp)));
                    sink = Sink::Param;
                    continue;
                }
                if (cmd == "return" || cmd == "returns" || cmd == "result") {
                    append(out.returns, rest);
                    sink = Sink::Returns;
                    continue;
                }
                if (const char *label = command_label(cmd)) {
                    notes.push_back(std::string(label) + ' ' + rest);
                    sink = Sink::Note;
                    continue;
                }
                if (is_ignored_command(cmd)) {
                    // "@brief text" is prose, just introduced by a command; the
                    // other ignored commands take their text with them.
                    if (cmd == "brief" || cmd == "details") {
                        body.push_back(rest);
                        sink = Sink::Body;
                    } else {
                        sink = Sink::Drop;
                    }
                    continue;
                }
                // An unknown command: keep its text as prose rather than lose it.
                body.push_back(rest);
                sink = Sink::Body;
                continue;
            }

            if (line.empty()) {
                // A blank line ends any command's text and starts a paragraph.
                if (sink != Sink::Body) {
                    sink = Sink::Body;
                }
                body.push_back("");
                continue;
            }

            switch (sink) {
            case Sink::Body:
                body.push_back(line);
                break;
            case Sink::Returns:
                append(out.returns, line);
                break;
            case Sink::Param:
                if (!out.params.empty()) {
                    append(out.params.back().second, line);
                }
                break;
            case Sink::Note:
                if (!notes.empty()) {
                    append(notes.back(), line);
                }
                break;
            case Sink::Drop:
                break;
            }
        }

        // Render: prose paragraphs as written, then the labelled notes.
        std::string text;
        for (const std::string &l : body) {
            if (l.empty()) {
                if (!text.empty() && text.substr(text.size() - 1) != "\n") {
                    text += '\n';
                }
                continue;
            }
            if (!text.empty() && text[text.size() - 1] != '\n') {
                text += ' ';
            }
            text += l;
        }
        // Trim first: a `@brief` followed by a blank line leaves a trailing
        // newline in the prose, and appending a paragraph separator to that
        // would open the note with two blank lines instead of one.
        while (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == ' ')) {
            text.erase(text.size() - 1);
        }
        for (const std::string &n : notes) {
            if (!text.empty()) {
                text += "\n\n";
            }
            text += n;
        }
        out.text = trim(text);
        return out;
    }

    // ---- default arguments -------------------------------------------------

    // Is `s` a default argument this tool is willing to re-emit VERBATIM into
    // generated C++ (as a `py::arg("x") = <s>`)?
    //
    // The bar is high on purpose. A wrong guess here does not produce a wrong
    // docstring, it produces a binding that does not compile — so the filter
    // takes only expressions whose meaning cannot depend on the surrounding
    // scope: literals, `{}`, and qualified-ids. An UNqualified enumerator
    // ("Fast" meaning "Shape::Mode::Fast") is rejected precisely because it
    // would not resolve where the binding spells it, and so is anything with a
    // call, an operator or a comma in it.
    std::string safe_default_text(const std::string &raw) {
        const std::string s = trim(squeeze(raw));
        if (s.empty()) {
            return {};
        }
        // String / character literals, which are the only accepted form that may
        // contain spaces — so they are tested before everything else.
        if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
            for (std::size_t i = 1; i + 1 < s.size(); ++i) {
                if (s[i] == '\\') {
                    ++i;
                    continue;
                }
                if (s[i] == s.front()) {
                    return {}; // two literals juxtaposed, or worse
                }
            }
            return s;
        }
        if (s == "{}" || s == "true" || s == "false" || s == "nullptr" || s == "NULL") {
            return s;
        }
        // ':' is absent from this set on purpose — it is what a qualified-id is
        // made of. A ternary is still excluded, by its '?'.
        if (s.find_first_of("(),\" '?+*/%&|^!<>[]{}") != std::string::npos) {
            return {}; // a call, an operator, an init-list with contents, ...
        }
        // A number: starts with a digit, a dot, or a sign followed by one.
        std::size_t k = (s[0] == '+' || s[0] == '-') ? 1 : 0;
        if (k < s.size() && (std::isdigit(static_cast<unsigned char>(s[k])) || s[k] == '.')) {
            for (std::size_t i = k; i < s.size(); ++i) {
                const char c = s[i];
                if (!is_ident_char(c) && c != '.' && c != '\'' && c != '-' && c != '+') {
                    return {};
                }
            }
            return s;
        }
        // A qualified-id — "Mode::Fast", "std::numeric_limits<double>::max" is
        // already out (it has '<'), a bare "Fast" is out (no "::").
        if (s.find("::") == std::string::npos) {
            return {};
        }
        for (char c : s) {
            if (!is_ident_char(c) && c != ':') {
                return {};
            }
        }
        return s;
    }

    // ---- parameter lists ---------------------------------------------------

    // Split a parameter list at its TOP-LEVEL commas — the ones not inside
    // (), [], {} or a template argument list. Getting `<>` right is what keeps
    // `std::map<int, double> m` from being read as two parameters.
    std::vector<std::string> split_params(const std::string &text) {
        std::vector<std::string> out;
        int                      paren = 0, brack = 0, brace = 0, angle = 0;
        std::size_t              start = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '"' || c == '\'') {
                skip_literal(text, i);
                --i;
                continue;
            }
            switch (c) {
            case '(':
                ++paren;
                break;
            case ')':
                --paren;
                break;
            case '[':
                ++brack;
                break;
            case ']':
                --brack;
                break;
            case '{':
                ++brace;
                break;
            case '}':
                --brace;
                break;
            case '<':
                ++angle;
                break;
            case '>':
                // Not a closing angle when it is part of "->" or ">=".
                if (angle > 0 && (i == 0 || text[i - 1] != '-')) {
                    --angle;
                }
                break;
            case ',':
                if (paren == 0 && brack == 0 && brace == 0 && angle == 0) {
                    out.push_back(text.substr(start, i - start));
                    start = i + 1;
                }
                break;
            default:
                break;
            }
        }
        out.push_back(text.substr(start));
        return out;
    }

    // The index of the top-level '=' that introduces a default argument, or npos.
    std::size_t default_pos(const std::string &s) {
        int paren = 0, brack = 0, brace = 0, angle = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '"' || c == '\'') {
                skip_literal(s, i);
                --i;
                continue;
            }
            switch (c) {
            case '(':
                ++paren;
                break;
            case ')':
                --paren;
                break;
            case '[':
                ++brack;
                break;
            case ']':
                --brack;
                break;
            case '{':
                ++brace;
                break;
            case '}':
                --brace;
                break;
            case '<':
                ++angle;
                break;
            case '>':
                if (angle > 0 && (i == 0 || s[i - 1] != '-')) {
                    --angle;
                }
                break;
            case '=':
                if (paren == 0 && brack == 0 && brace == 0 && angle == 0) {
                    // Not "==", "<=", ">=", "!=" — none of which can legally
                    // appear here, but cheap to exclude.
                    const bool op = (i + 1 < s.size() && s[i + 1] == '=') ||
                                    (i > 0 && (s[i - 1] == '=' || s[i - 1] == '<' ||
                                               s[i - 1] == '>' || s[i - 1] == '!'));
                    if (!op) {
                        return i;
                    }
                }
                break;
            default:
                break;
            }
        }
        return std::string::npos;
    }

    // The ':' that introduces a bit-field width (`int flags : 3`), or npos.
    //
    // A plain `s.find(':')` is wrong and quietly so: it also finds the first
    // colon of the `::` in a qualified type, so `std::string title;` was cut
    // down to `std` and the member vanished — silently, and only for members
    // whose type is qualified and which have no initializer to cut at first.
    // A bit-field colon is a SINGLE colon, and it is not inside a template
    // argument list either (`std::map<int, A::B> m`).
    std::size_t bitfield_colon(const std::string &s) {
        int angle = 0, paren = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '"' || c == '\'') {
                skip_literal(s, i);
                --i;
                continue;
            }
            if (c == '<') {
                ++angle;
            } else if (c == '>') {
                if (angle > 0 && (i == 0 || s[i - 1] != '-')) {
                    --angle;
                }
            } else if (c == '(') {
                ++paren;
            } else if (c == ')') {
                --paren;
            } else if (c == ':' && angle == 0 && paren == 0) {
                const bool doubled =
                    (i + 1 < s.size() && s[i + 1] == ':') || (i > 0 && s[i - 1] == ':');
                if (!doubled) {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    // One parameter's declarator -> its name, or "" when it declares no name.
    std::string param_name_of(const std::string &declarator) {
        std::string d = trim(declarator);
        if (d.empty() || d == "void" || d == "...") {
            return {};
        }
        // An array declarator: "double v[3]" names v.
        while (!d.empty() && d.back() == ']') {
            const std::size_t open = d.rfind('[');
            if (open == std::string::npos) {
                return {};
            }
            d = trim(d.substr(0, open));
        }
        // A function-pointer or member-pointer parameter ends in ')': its name
        // is buried inside the declarator, and this tool does not go there.
        if (!d.empty() && d.back() == ')') {
            return {};
        }
        std::size_t       begin = 0;
        const std::string name  = trailing_identifier(d, &begin);
        if (name.empty() || is_keyword(name)) {
            return {};
        }
        // "std::string" would otherwise yield "string": a '::' immediately
        // before the identifier makes it the tail of a qualified TYPE.
        std::string before = trim(d.substr(0, begin));
        if (before.size() >= 2 && before.compare(before.size() - 2, 2, "::") == 0) {
            return {};
        }
        if (before.empty()) {
            return {}; // just a type: "Mode"
        }
        // Only cv-qualifiers / type introducers in front: still just a type.
        std::istringstream in(before);
        std::string        w;
        bool               only_introducers = true;
        while (in >> w) {
            if (!is_type_introducer(w)) {
                only_introducers = false;
                break;
            }
        }
        return only_introducers ? std::string() : name;
    }

    std::vector<DocParamInfo> parse_param_list(const std::string &text) {
        std::vector<DocParamInfo> out;
        if (trim(text).empty() || trim(squeeze(text)) == "void") {
            return out;
        }
        for (const std::string &piece : split_params(text)) {
            const std::string p = trim(piece);
            if (p.empty()) {
                continue;
            }
            DocParamInfo      info;
            const std::size_t eq = default_pos(p);
            if (eq == std::string::npos) {
                info.name = param_name_of(p);
            } else {
                info.name         = param_name_of(p.substr(0, eq));
                info.default_text = safe_default_text(p.substr(eq + 1));
            }
            out.push_back(info);
        }
        return out;
    }

    // ---- 3. the scanner ----------------------------------------------------

    struct Scope {
        std::string name;
        bool        is_class  = false;
        bool        is_public = true; // current access, for a class scope
        int         depth     = 0;    // brace depth at which the scope was opened
    };

    class Scanner {
    public:
        Scanner(const std::string &src, DocMap &out) : s_(src), out_(out) {}

        void run() {
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (is_space(c)) {
                    ++i_;
                    continue;
                }
                if (c == '#' && at_line_start()) {
                    skip_pp_line(s_, i_);
                    continue;
                }
                if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                    line_comment();
                    continue;
                }
                if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                    block_comment();
                    continue;
                }
                if (c == '"' || c == '\'') {
                    skip_literal(s_, i_);
                    continue;
                }
                if (c == '{') {
                    ++i_;
                    ++depth_;
                    continue;
                }
                if (c == '}') {
                    ++i_;
                    --depth_;
                    pop_scopes();
                    pending_.clear();
                    continue;
                }
                if (c == ';') {
                    ++i_;
                    pending_.clear();
                    continue;
                }
                if (is_ident_start(c)) {
                    word();
                    continue;
                }
                ++i_;
            }
        }

    private:
        const std::string &s_;
        DocMap            &out_;
        std::size_t        i_     = 0;
        int                depth_ = 0;
        std::vector<Scope> scopes_;
        std::string        pending_; // the doc comment awaiting a declaration

        bool at_line_start() const {
            for (std::size_t j = i_; j-- > 0;) {
                if (s_[j] == '\n') {
                    return true;
                }
                if (!is_space(s_[j])) {
                    return false;
                }
            }
            return true;
        }

        // The "a::b::C" prefix the current scopes spell.
        std::string scope_prefix() const {
            std::string out;
            for (const Scope &sc : scopes_) {
                if (sc.name.empty()) {
                    continue;
                }
                if (!out.empty()) {
                    out += "::";
                }
                out += sc.name;
            }
            return out;
        }

        // Same, but only the innermost CLASS and what encloses it — the key a
        // manifest that names the class unqualified would look under.
        std::string class_only_prefix() const {
            for (std::size_t k = scopes_.size(); k-- > 0;) {
                if (scopes_[k].is_class) {
                    return scopes_[k].name;
                }
            }
            return {};
        }

        bool in_class() const {
            for (std::size_t k = scopes_.size(); k-- > 0;) {
                if (scopes_[k].is_class) {
                    return true;
                }
            }
            return false;
        }

        bool member_is_public() const {
            for (std::size_t k = scopes_.size(); k-- > 0;) {
                if (scopes_[k].is_class) {
                    return scopes_[k].is_public;
                }
            }
            return true;
        }

        void pop_scopes() {
            while (!scopes_.empty() && scopes_.back().depth > depth_) {
                scopes_.pop_back();
            }
        }

        void set_access(bool pub) {
            for (std::size_t k = scopes_.size(); k-- > 0;) {
                if (scopes_[k].is_class) {
                    scopes_[k].is_public = pub;
                    return;
                }
            }
        }

        // ---- comments -----------------------------------------------------

        void line_comment() {
            const std::size_t start = i_;
            i_ += 2;
            const bool doc      = i_ < s_.size() && (s_[i_] == '/' || s_[i_] == '!');
            bool       trailing = false;
            if (doc) {
                ++i_;
                if (i_ < s_.size() && s_[i_] == '<') {
                    ++i_;
                    trailing = true;
                }
            }
            const std::size_t text_start = i_;
            while (i_ < s_.size() && s_[i_] != '\n') {
                ++i_;
            }
            if (!doc) {
                return;
            }
            const std::string text = s_.substr(text_start, i_ - text_start);
            if (trailing) {
                attach_trailing(text, start);
                return;
            }
            // Consecutive `///` lines are one block. They are one block only if
            // nothing but whitespace separated them, which is true by
            // construction: the main loop would have consumed any token.
            if (!pending_.empty() && pending_end_ == start_of_line(start)) {
                pending_ += '\n';
            } else if (!pending_.empty()) {
                pending_ = std::string();
            }
            pending_ += text;
            pending_end_ = i_ + 1; // the line after this one
        }

        // The offset of the first character of the line containing `pos`.
        std::size_t start_of_line(std::size_t pos) const {
            while (pos > 0 && s_[pos - 1] != '\n') {
                --pos;
            }
            return pos;
        }

        void block_comment() {
            i_ += 2;
            const bool        doc   = i_ < s_.size() && (s_[i_] == '*' || s_[i_] == '!') &&
                                      !(i_ + 1 < s_.size() && s_[i_ + 1] == '/');
            const std::size_t start = i_ + (doc ? 1 : 0);
            const std::size_t end   = s_.find("*/", i_);
            const std::size_t stop  = end == std::string::npos ? s_.size() : end;
            if (doc) {
                pending_     = s_.substr(start, stop - start);
                pending_end_ = stop + 2;
            }
            i_ = end == std::string::npos ? s_.size() : end + 2;
        }

        // A `///<` comment documents the declaration it sits BEHIND, so it is
        // only ever the one that ended on this same line. A field with nothing
        // but a trailing comment carries no documentation at the moment it is
        // read, and so was not recorded — hence the skeleton kept aside by
        // record_declaration, which this promotes into a real entry.
        void attach_trailing(const std::string &text, std::size_t comment_start) {
            if (trail_end_ == std::string::npos ||
                start_of_line(trail_end_) != start_of_line(comment_start)) {
                return;
            }
            const std::string doc = parse_doxygen(text).text;
            if (doc.empty()) {
                return;
            }
            if (trail_recorded_) {
                // Only fill a blank: a doc comment ABOVE the member is the
                // author's main statement, and a trailing note does not replace it.
                for (std::size_t k = 0; k < 2; ++k) {
                    const std::string &key = k == 0 ? trail_full_ : trail_short_;
                    const std::size_t  idx = k == 0 ? trail_full_idx_ : trail_short_idx_;
                    if (key.empty()) {
                        continue;
                    }
                    std::vector<DocEntryInfo> &v = out_[key];
                    if (idx < v.size() && v[idx].doc.empty()) {
                        v[idx].doc = doc;
                    }
                }
                return;
            }
            trail_entry_.doc = doc;
            record(trail_cls_, trail_entry_, trail_member_);
            trail_recorded_ = true;
        }

        std::size_t pending_end_ = 0;

        // The declaration a `///<` on this line would document: where it ended,
        // how it is keyed, and — when it carried nothing worth recording at the
        // time — the entry that would be stored if a trailing comment arrives.
        std::size_t  trail_end_ = std::string::npos;
        std::string  trail_cls_;
        std::string  trail_member_;
        std::string  trail_full_;
        std::string  trail_short_;
        std::size_t  trail_full_idx_  = 0;
        std::size_t  trail_short_idx_ = 0;
        bool         trail_recorded_  = false;
        DocEntryInfo trail_entry_;

        // ---- keywords and declarations ------------------------------------

        std::string read_word() {
            const std::size_t b = i_;
            while (i_ < s_.size() && is_ident_char(s_[i_])) {
                ++i_;
            }
            return s_.substr(b, i_ - b);
        }

        // Whitespace ONLY — deliberately not skip_ws(), which also steps over
        // comments. Used where the very next thing may be the doc comment of
        // the NEXT declaration: eating it there loses it silently, and the
        // member that should have carried it ends up undocumented while its
        // neighbour looks fine. See the call after an inline function body.
        void skip_blanks() {
            while (i_ < s_.size() && is_space(s_[i_])) {
                ++i_;
            }
        }

        void skip_ws() {
            while (i_ < s_.size()) {
                if (is_space(s_[i_])) {
                    ++i_;
                    continue;
                }
                if (s_[i_] == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                    while (i_ < s_.size() && s_[i_] != '\n') {
                        ++i_;
                    }
                    continue;
                }
                if (s_[i_] == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                    const std::size_t end = s_.find("*/", i_ + 2);
                    i_                    = end == std::string::npos ? s_.size() : end + 2;
                    continue;
                }
                return;
            }
        }

        // Skip a balanced run that starts at s_[i_] == open.
        void skip_balanced(char open, char close) {
            if (i_ >= s_.size() || s_[i_] != open) {
                return;
            }
            int d = 0;
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (c == '"' || c == '\'') {
                    skip_literal(s_, i_);
                    continue;
                }
                if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                    while (i_ < s_.size() && s_[i_] != '\n') {
                        ++i_;
                    }
                    continue;
                }
                if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                    const std::size_t end = s_.find("*/", i_ + 2);
                    i_                    = end == std::string::npos ? s_.size() : end + 2;
                    continue;
                }
                if (c == open) {
                    ++d;
                } else if (c == close) {
                    --d;
                    if (d == 0) {
                        ++i_;
                        return;
                    }
                }
                ++i_;
            }
        }

        void skip_to_semicolon() {
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (c == '"' || c == '\'') {
                    skip_literal(s_, i_);
                    continue;
                }
                if (c == '{') {
                    skip_balanced('{', '}');
                    continue;
                }
                if (c == ';') {
                    ++i_;
                    return;
                }
                ++i_;
            }
        }

        void word() {
            const std::size_t start = i_;
            const std::string w     = read_word();

            if (w == "namespace") {
                namespace_decl();
                return;
            }
            if (w == "class" || w == "struct" || w == "union") {
                class_decl(w != "class");
                return;
            }
            if (w == "enum") {
                pending_.clear();
                skip_to_semicolon();
                return;
            }
            if (w == "template") {
                skip_ws();
                if (i_ < s_.size() && s_[i_] == '<') {
                    skip_balanced('<', '>');
                }
                return; // the declaration itself is handled next time round
            }
            if (w == "public" || w == "protected" || w == "private") {
                const std::size_t save = i_;
                skip_ws();
                if (i_ < s_.size() && s_[i_] == ':') {
                    ++i_;
                    set_access(w == "public");
                    return;
                }
                i_ = save; // a base-clause specifier, not an access label
                return;
            }
            if (w == "using" || w == "typedef" || w == "friend" || w == "static_assert") {
                pending_.clear();
                skip_to_semicolon();
                return;
            }
            if (w == "extern") {
                // extern "C" { ... }: keep scanning inside it, so drop the
                // linkage spec and let the loop meet the brace.
                skip_ws();
                if (i_ < s_.size() && s_[i_] == '"') {
                    skip_literal(s_, i_);
                }
                return;
            }

            i_ = start;
            declaration();
        }

        void namespace_decl() {
            skip_ws();
            std::vector<std::string> parts;
            while (i_ < s_.size() && is_ident_start(s_[i_])) {
                const std::string n = read_word();
                if (n == "inline") { // inline namespace
                    skip_ws();
                    continue;
                }
                parts.push_back(n);
                skip_ws();
                if (i_ + 1 < s_.size() && s_[i_] == ':' && s_[i_ + 1] == ':') {
                    i_ += 2;
                    skip_ws();
                    continue;
                }
                break;
            }
            skip_ws();
            if (i_ < s_.size() && s_[i_] == '=') {
                pending_.clear();
                skip_to_semicolon(); // namespace alias
                return;
            }
            if (i_ < s_.size() && s_[i_] == '{') {
                ++i_;
                ++depth_;
                if (parts.empty()) {
                    parts.push_back(std::string()); // anonymous
                }
                for (const std::string &p : parts) {
                    Scope sc;
                    sc.name  = p;
                    sc.depth = depth_;
                    scopes_.push_back(sc);
                }
            }
            pending_.clear();
        }

        void class_decl(bool default_public) {
            const std::string doc = pending_;
            pending_.clear();
            skip_ws();
            // Attributes and dllexport-style macros between the keyword and the
            // name. A macro is just an identifier here; the LAST identifier
            // before '{', ':' or ';' is the class name.
            std::string name;
            while (i_ < s_.size()) {
                if (s_[i_] == '[' && i_ + 1 < s_.size() && s_[i_ + 1] == '[') {
                    skip_balanced('[', ']');
                    skip_ws();
                    continue;
                }
                if (is_ident_start(s_[i_])) {
                    name = read_word();
                    skip_ws();
                    continue;
                }
                if (s_[i_] == '(') { // __declspec(...) / macro arguments
                    skip_balanced('(', ')');
                    skip_ws();
                    continue;
                }
                break;
            }
            if (i_ < s_.size() && s_[i_] == ';') {
                ++i_; // forward declaration
                return;
            }
            if (i_ < s_.size() && s_[i_] == ':') {
                // Base-clause: run to the opening brace.
                while (i_ < s_.size() && s_[i_] != '{' && s_[i_] != ';') {
                    if (s_[i_] == '<') {
                        skip_balanced('<', '>');
                        continue;
                    }
                    ++i_;
                }
            }
            if (i_ >= s_.size() || s_[i_] != '{') {
                return;
            }
            ++i_;
            ++depth_;
            Scope sc;
            sc.name      = name;
            sc.is_class  = true;
            sc.is_public = default_public;
            sc.depth     = depth_;
            scopes_.push_back(sc);
            if (!doc.empty() && !name.empty()) {
                DocEntryInfo e;
                e.doc = parse_doxygen(doc).text;
                if (!e.doc.empty()) {
                    record(name, e, /*member=*/std::string());
                }
            }
        }

        // Read one declaration: everything up to the ';' or '{' that ends it.
        void declaration() {
            const std::size_t start = i_;
            int               paren = 0, brack = 0;
            bool              body = false;
            while (i_ < s_.size()) {
                const char c = s_[i_];
                if (c == '"' || c == '\'') {
                    skip_literal(s_, i_);
                    continue;
                }
                if (c == '/' && i_ + 1 < s_.size() && (s_[i_ + 1] == '/' || s_[i_ + 1] == '*')) {
                    skip_ws();
                    continue;
                }
                if (c == '(') {
                    ++paren;
                } else if (c == ')') {
                    --paren;
                } else if (c == '[') {
                    ++brack;
                } else if (c == ']') {
                    --brack;
                } else if (paren == 0 && brack == 0 && (c == ';' || c == '{')) {
                    body = (c == '{');
                    break;
                }
                ++i_;
            }
            const std::string decl = s_.substr(start, i_ - start);
            if (body) {
                skip_balanced('{', '}');
                // Only blanks: a `/// ...` on the next line belongs to the next
                // member, not to this trailing semicolon.
                skip_blanks();
                if (i_ < s_.size() && s_[i_] == ';') {
                    ++i_;
                }
            } else if (i_ < s_.size()) {
                ++i_; // the ';'
            }
            const std::string doc = pending_;
            pending_.clear();
            record_declaration(decl, doc);
        }

        // ---- turning declaration text into an entry -----------------------

        void record_declaration(const std::string &decl_in, const std::string &doc) {
            const std::string decl = squeeze(decl_in);
            if (decl.empty()) {
                return;
            }
            // A member of a class is only bindable when it is public; a
            // namespace-scope declaration is a free function candidate.
            if (in_class() && !member_is_public()) {
                return;
            }
            // Operators and conversion functions have no bindable name — the
            // walk drops them, so documenting them would document nothing.
            if (decl.find("operator") != std::string::npos) {
                return;
            }
            // An inline annotation on the member is the author speaking
            // directly; a harvested comment must not silently outrank it. The
            // annotation is right here in the text, so the check is exact.
            if (decl.find("rosetta::doc") != std::string::npos ||
                decl.find("= doc{") != std::string::npos) {
                return;
            }

            const std::size_t lp = top_level_paren(decl);
            DocEntryInfo      e;
            std::string       name;
            if (lp == std::string::npos) {
                // A data member: cut at the initializer or the bit-field width.
                std::string       d  = decl;
                const std::size_t eq = default_pos(d);
                if (eq != std::string::npos) {
                    d = d.substr(0, eq);
                }
                const std::size_t colon = bitfield_colon(d);
                if (colon != std::string::npos) {
                    d = d.substr(0, colon);
                }
                name = param_name_of(d);
                if (name.empty()) {
                    return;
                }
                e.is_function = false;
            } else {
                const std::size_t rp = matching_paren(decl, lp);
                if (rp == std::string::npos) {
                    return;
                }
                std::size_t       nb   = 0;
                const std::string head = decl.substr(0, lp);
                name                   = trailing_identifier(head, &nb);
                if (name.empty() || is_keyword(name)) {
                    return;
                }
                // A constructor or destructor: the IR has no doc slot for either.
                if (name == class_only_prefix()) {
                    return;
                }
                const std::string before = trim(head.substr(0, nb));
                if (before.empty() ||
                    (before.size() >= 2 && before.compare(before.size() - 2, 2, "::") == 0)) {
                    return; // a call expression or an out-of-line definition
                }
                e.is_function = true;
                e.params      = parse_param_list(decl.substr(lp + 1, rp - lp - 1));
            }

            const ParsedComment pc = parse_doxygen(doc);
            e.doc                  = pc.text;
            e.returns              = pc.returns;
            // Match each @param to the declaration's own parameter names. A
            // @param naming something the signature does not have is dropped:
            // it is stale documentation, and guessing a position for it would
            // put the wrong text on the wrong argument.
            for (const std::pair<std::string, std::string> &pd : pc.params) {
                for (DocParamInfo &p : e.params) {
                    if (p.name == pd.first) {
                        p.doc = pd.second;
                        break;
                    }
                }
            }
            // Remember this declaration as the target a `///<` on the same line
            // would document, whether or not it is recorded below.
            trail_end_      = i_;
            trail_cls_      = class_only_prefix();
            trail_member_   = name;
            trail_entry_    = e;
            trail_recorded_ = false;

            // Nothing worth carrying: no comment, no default argument, and
            // (for the overload-selection path) no names either.
            bool useful = !e.doc.empty() || !e.returns.empty();
            for (const DocParamInfo &p : e.params) {
                useful = useful || !p.doc.empty() || !p.default_text.empty() || !p.name.empty();
            }
            if (!useful) {
                return;
            }
            record(trail_cls_, e, name);
            trail_recorded_ = true;
        }

        // Store under the fully-qualified key and, for a class inside a
        // namespace, the unqualified one too — a manifest may spell either.
        void record(const std::string &cls, const DocEntryInfo &e, const std::string &member) {
            const std::string prefix = scope_prefix();
            const std::string suffix = member.empty() ? std::string() : "::" + member;
            std::string       full;
            if (member.empty()) {
                // A class's own comment: the key is the class, and
                // scope_prefix() already ends with it.
                full = prefix;
            } else {
                full = prefix.empty() ? member : prefix + suffix;
            }
            if (full.empty()) {
                return;
            }
            out_[full].push_back(e);
            trail_full_     = full;
            trail_full_idx_ = out_[full].size() - 1;

            const std::string shortk = member.empty() ? cls : (cls.empty() ? member : cls + suffix);
            trail_short_.clear();
            if (!shortk.empty() && shortk != full) {
                out_[shortk].push_back(e);
                trail_short_     = shortk;
                trail_short_idx_ = out_[shortk].size() - 1;
            }
        }

        // The first '(' of `s` that is not inside a template argument list —
        // "std::function<void(int)> cb" must not be read as a function.
        static std::size_t top_level_paren(const std::string &s) {
            int angle = 0, brack = 0;
            for (std::size_t i = 0; i < s.size(); ++i) {
                const char c = s[i];
                if (c == '"' || c == '\'') {
                    skip_literal(s, i);
                    --i;
                    continue;
                }
                if (c == '<') {
                    ++angle;
                } else if (c == '>') {
                    if (angle > 0 && (i == 0 || s[i - 1] != '-')) {
                        --angle;
                    }
                } else if (c == '[') {
                    ++brack;
                } else if (c == ']') {
                    --brack;
                } else if (c == '(' && angle == 0 && brack == 0) {
                    return i;
                }
            }
            return std::string::npos;
        }

        static std::size_t matching_paren(const std::string &s, std::size_t open) {
            int d = 0;
            for (std::size_t i = open; i < s.size(); ++i) {
                const char c = s[i];
                if (c == '"' || c == '\'') {
                    skip_literal(s, i);
                    --i;
                    continue;
                }
                if (c == '(') {
                    ++d;
                } else if (c == ')') {
                    --d;
                    if (d == 0) {
                        return i;
                    }
                }
            }
            return std::string::npos;
        }
    };

} // namespace

DocMap harvest_doc_comments(const std::string &source) {
    DocMap  out;
    Scanner sc(source, out);
    sc.run();
    return out;
}

DocMap harvest_doc_comments_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return harvest_doc_comments(ss.str());
}

void merge_doc_maps(DocMap &into, const DocMap &from) {
    for (std::map<std::string, std::vector<DocEntryInfo>>::const_iterator it = from.begin();
         it != from.end(); ++it) {
        std::vector<DocEntryInfo> &dst = into[it->first];
        dst.insert(dst.end(), it->second.begin(), it->second.end());
    }
}
