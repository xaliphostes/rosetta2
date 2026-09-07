// Copyright (c) fmaerten@gmail.com
// License: MIT

// Doc-comment harvesting: read the documentation an existing library already
// has, out of its headers, so a generated binding is not born undocumented.
//
// Why this lives in the TOOL and not in the reflection walk: P2996 reflects
// declarations, not comments. `std::meta` can tell you that `Shape::area` takes
// a `double radius` with a default argument, but not that the line above it
// says what a radius means, and not what the default's expression was. Both are
// in the header text, and rosetta_gen already reads the header tree, so this is
// a small lexical pass over the same files rather than a new dependency.
//
// What it produces is DESCRIPTIVE only. Nothing here can add, remove or rename
// a binding: the harvest is matched against the reflected signature afterwards
// (see apply_doc_comments in rosetta/inline/generate.hxx) and dropped whenever
// the two disagree, so a mis-parsed declaration loses its documentation rather
// than attaching it to the wrong member.
//
// Scope, deliberately: this is a scanner, not a C++ parser. It tracks
// namespaces, class/struct bodies, access specifiers, comments, string and
// character literals (raw strings included) and preprocessor lines, and reads
// declarations well enough to get a member's name, its parameter names and its
// default arguments. It does NOT expand macros, follow #include, or understand
// templates beyond skipping them. Everything it cannot read confidently it
// declines to record — see safe_default_text, the most conservative part.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// One parameter of a harvested declaration.
struct DocParamInfo {
    std::string name;         // as the header spells it; "" when unnamed
    std::string doc;          // the matching @param text, if any
    std::string default_text; // the default argument's spelling; "" if none/unsafe
};

// One harvested declaration: its comment, and what the declaration itself says.
struct DocEntryInfo {
    std::string               doc;     // brief + detail + notes, rendered
    std::string               returns; // @return / @returns text
    std::vector<DocParamInfo> params;  // in declaration order
    bool                      is_function = false;
};

// Harvested documentation, keyed by "Scope::member" — fully qualified, and
// additionally under the unqualified "Class::member" when the class sits in a
// namespace, because a manifest may name a class either way. A class's own
// comment is stored under the class spelling with no member suffix.
//
// The value is a LIST: a name can be overloaded, and which entry belongs to
// which binding is decided later, against the reflected signature.
using DocMap = std::map<std::string, std::vector<DocEntryInfo>>;

// Scan one header's TEXT. Never throws: anything unparseable is skipped.
DocMap harvest_doc_comments(const std::string &source);

// Scan one header FILE. Returns an empty map when it cannot be read, which is
// the right answer for a header that is generated, absent, or outside the
// include roots — harvesting is a bonus, never a build error.
DocMap harvest_doc_comments_file(const fs::path &p);

// Append `from`'s entries into `into`, preserving per-key declaration order.
void merge_doc_maps(DocMap &into, const DocMap &from);
