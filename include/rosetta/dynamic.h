// Copyright (c) fmaerten@gmail.com
// License: MIT

// rosetta's IR, alive at run time — the dynamic object model.
//
// Everything here is a runtime mirror of the compile-time IR in
// <rosetta/generate.h>: TypeDesc mirrors GenType, MetaField mirrors GenField,
// MetaMethod mirrors GenMethod, MetaClass mirrors GenClass. The `dynamic`
// backend emits one static instance of each, plus a thunk per member, so a
// program can ask "what classes exist, what can they do, call this one by
// name" with no code generated for the *caller*.
//
// Why this header is deliberately boring C++:
//
//   * NO reflection, NO <experimental/meta>, NO dependency on generate.h.
//     It is compiled into the TARGET, next to the generated tables, exactly
//     like the -expanded backends' runtime headers (runtime/node.h,
//     runtime/qt_widgets.h). The generation host still needs C++26; the
//     target needs a stock C++20 compiler.
//
//   * The tables are static aggregates of trivially-constructible data —
//     const char*, pointer-and-count spans, function pointers. No std::string,
//     no std::vector, no dynamic initialization at load time. A 500-class
//     library's metadata lands in .rodata, not in a static constructor.
//
// What it buys, in the order the motivation usually gets stated:
//
//   1. UI built by query, not by codegen. Walk registry().classes(), read
//      MetaField::range / choices / readonly / doc, emit widgets. This is what
//      visitors/qml_reflected_object.h, runtime/imgui.h and
//      runtime/qt_widgets.h each hand-roll today against a different
//      Any type (QVariant, ImGui state, JSON).
//
//   2. One wrapper per language instead of one per class. A Python or Lua
//      binding becomes ~300 lines that speak Any/ArgList, not N generated
//      registration blocks.
//
//   3. Overloads come back. MetaClass keeps the whole overload set and
//      resolve() scores candidates against the actual argument types — so the
//      name-keyed targets (node, wasm, C#, Java, REST, Lua) stop losing every
//      sibling but the first. See docs/COVERAGE.md for what they drop today.
//
// The load-bearing decision is Any's CANONICAL REPRESENTATION (see Any below):
// a value is stored as one of six canonical C++ types, never as the exact
// source type. That is what lets a language wrapper read any value without
// knowing whether the C++ side said int, long, size_t or uint32_t — and it is
// the difference between "one wrapper per language" and "a wrapper that has to
// re-derive the type lattice".
//
// Declarations only; the bodies live in inline/dynamic.hxx.

#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rosetta::dyn {

    struct MetaClass;
    struct TypeDesc;
    class Any;
    class ArgList;

    // ---------------------------------------------------------------------
    // Types
    // ---------------------------------------------------------------------

    /**
     * @brief What a type IS, coarsely — the runtime spelling of GenType::kind.
     *
     * The enumerators track the strings type_descriptor() produces in
     * inline/generate.hxx ("void", "boolean", "string", "number", "vector",
     * "enum", "object", "unknown") one for one, on purpose: the emitter maps
     * kind-string to enumerator with a lookup and nothing else, so the dynamic
     * path and the expanded path can never disagree about what a type is.
     *
     * `unknown` is not an error — it is rosetta's honest "no backend gate
     * claimed this". A member whose type is unknown still appears in the
     * metadata (so a UI can grey it out and a coverage diff can see it) but its
     * thunk is null and calling it fails with a stated reason, rather than the
     * member vanishing the way it does from a generated binding today.
     */
    enum class Kind : std::uint8_t {
        unknown = 0,
        void_,
        boolean,
        number,
        string,
        enum_,
        object,
        vector,
    };

    const char *kind_name(Kind k);

    /** @brief One enumerator of an enum type, for combobox/menu construction. */
    struct MetaEnumerator {
        const char *name  = "";
        long long   value = 0;
    };

    /**
     * @brief Runtime GenType. Recursive through `element`, exactly as GenType
     * is through GenType::element, so "vector of vector of bound class Foo" is
     * describable — which is the reason this is not a std::type_index. A
     * type_index answers "are these the same type"; a UI and a marshaller need
     * "what is inside it", and only a structural descriptor answers that.
     *
     * Emitted as static const objects and shared by address: two members of the
     * same type point at the same TypeDesc, so `a.type() == b.type()` is a
     * pointer compare in the common case (see same_type() for the general one).
     */
    struct TypeDesc {
        Kind        kind      = Kind::unknown;
        const char *spelling  = ""; // prettified C++ spelling, for humans and errors
        const char *object    = ""; // qualified class / enum name when kind is object|enum_
        bool        integral  = false; // kind == number and integral (vs floating)
        bool        is_pointer     = false; // T* to a bound class
        bool        is_shared_ptr  = false; // std::shared_ptr<T>; `element` is the pointee
        const TypeDesc *element    = nullptr; // vector element / shared_ptr pointee

        const MetaEnumerator *enumerators   = nullptr; // kind == enum_
        std::size_t           n_enumerators = 0;

        // Set for kind == object: the class's metadata, once registered. Filled
        // by the generated registration function rather than at static-init
        // time, because two classes can reference each other's TypeDesc.
        const MetaClass *cls = nullptr;
    };

    /** @brief Structural equality — used by overload scoring, not by identity. */
    bool same_type(const TypeDesc *a, const TypeDesc *b);

    // ---------------------------------------------------------------------
    // Any
    // ---------------------------------------------------------------------

    /**
     * @brief A value plus the rosetta type that describes it.
     *
     * CANONICAL REPRESENTATION — the whole design rests on this. Whatever the
     * C++ source type was, the box holds exactly one of:
     *
     *     Kind::boolean  -> bool
     *     Kind::number   -> long long   (integral == true)
     *                       double      (integral == false)
     *     Kind::string   -> std::string
     *     Kind::enum_    -> long long
     *     Kind::vector   -> std::vector<Any>
     *     Kind::object   -> ObjectRef
     *     Kind::void_    -> (empty)
     *     Kind::unknown  -> whatever the emitter put there, opaque
     *
     * So a wrapper reads a number with as_number() without caring that the C++
     * signature said `std::size_t`, and an int-vs-size_t change in the bound
     * library is not an ABI break for the wrapper. `type()` still carries the
     * exact spelling for error messages and for round-tripping back into C++.
     *
     * std::any is the box because it already has the small-object optimization
     * and value semantics; it is NOT the type system. Reading it directly is
     * possible (raw()) but every marshaller should go through the as_* / cast
     * accessors so the canonical contract stays enforceable in one place.
     */
    struct ObjectRef {
        void            *ptr = nullptr;
        const MetaClass *cls = nullptr;

        // Non-null when this reference OWNS the object: the last Any to drop it
        // destroys it. Null means BORROWED — the object belongs to someone else
        // and outliving it is the caller's problem.
        //
        // This is the dangling policy rosetta does not currently state anywhere
        // (docs/MAIN-TODO.md §2). Making it one field of one struct is most of
        // the point: a borrowed sub-object handed out by a getter can PIN its
        // parent by carrying the parent's owner here, which is exactly what
        // node's Wrap<T>-stores-by-value and wasm's raw-pointer-return cannot
        // express today.
        std::shared_ptr<void> owner;
    };

    class Any {
    public:
        Any() = default;

        static Any none();
        static Any boolean(bool v);
        static Any integer(long long v, const TypeDesc *t = nullptr);
        static Any real(double v, const TypeDesc *t = nullptr);
        static Any text(std::string v, const TypeDesc *t = nullptr);
        static Any enumeration(long long v, const TypeDesc *t);
        // `t` defaults like integer() / real() / text()'s: a host-language
        // sequence has no single C++ element type to name, and list() supplies
        // a Kind::vector descriptor so the value still matches a vector
        // parameter. enumeration() deliberately has NO default — an enum
        // without its descriptor cannot name its enumerators.
        static Any list(std::vector<Any> v, const TypeDesc *t = nullptr);
        static Any object(void *p, const MetaClass *c, std::shared_ptr<void> owner = {},
                          const TypeDesc *t = nullptr);
        static Any opaque(std::any v, const TypeDesc *t);

        const TypeDesc *type() const { return type_; }
        Kind            kind() const;
        bool            empty() const { return !box_.has_value(); }

        // Canonical accessors. Each throws rosetta::dyn::Error when the box does
        // not hold the canonical type for its kind — which can only happen if a
        // hand-written Any broke the contract, never from generated code.
        bool                    as_bool() const;
        long long               as_int() const;
        double                  as_number() const; // integral values widen
        const std::string      &as_string() const;
        const std::vector<Any> &as_list() const;
        const ObjectRef        &as_object() const;

        const std::any &raw() const { return box_; }
        std::any       &raw() { return box_; }

        /** @brief Human-readable rendering, for error text and debug UIs. */
        std::string to_string() const;

    private:
        Any(const TypeDesc *t, std::any b) : type_(t), box_(std::move(b)) {}

        const TypeDesc *type_ = nullptr;
        std::any        box_;
    };

    /** @brief Thrown by the as_* accessors and by the throwing invoke() forms. */
    class Error : public std::exception {
    public:
        explicit Error(std::string what) : what_(std::move(what)) {}
        const char *what() const noexcept override { return what_.c_str(); }

    private:
        std::string what_;
    };

    // ---------------------------------------------------------------------
    // ArgList
    // ---------------------------------------------------------------------

    /**
     * @brief A call's arguments — positional, with optional names.
     *
     * Positional is the primary form because rosetta does not have parameter
     * names: GenParam::name is a synthesized "argN" (see the note at
     * include/rosetta/generate.h:291, and docs/MAIN-TODO.md §1). Names are
     * carried anyway so that the day parameter names DO get reflected — or when
     * they come from the manifest — keyword arguments and generated dialogs
     * with labelled fields work without an ArgList redesign.
     */
    class ArgList {
    public:
        ArgList() = default;
        ArgList(std::initializer_list<Any> vs);

        std::size_t size() const { return values_.size(); }
        bool        empty() const { return values_.empty(); }

        const Any &operator[](std::size_t i) const;
        const Any *by_name(std::string_view n) const;

        void add(Any v);
        void add(std::string name, Any v);

        const std::vector<Any> &values() const { return values_; }

    private:
        std::vector<Any>         values_;
        std::vector<std::string> names_; // parallel to values_ where known, "" otherwise
    };

    // ---------------------------------------------------------------------
    // Members
    // ---------------------------------------------------------------------

    /** @brief A free-form annotation the core does not interpret.
     *
     * The compile-time IR type-erases annotations into std::vector<std::any>
     * and lets a backend std::any_cast the kinds it knows (see
     * gen_detail::find_annotation). That does not survive to run time across a
     * language boundary — the caller has no type to cast to — so the dynamic
     * model flattens each annotation to a key and a JSON-ish value string. The
     * kinds a UI actually needs (doc / readonly / range / choices) get real
     * fields on MetaField; everything else rides here.
     */
    struct MetaAnnotation {
        const char *key   = "";
        const char *value = ""; // JSON scalar or object, as text
    };

    struct MetaRange {
        bool   has = false;
        double lo  = 0;
        double hi  = 0;
    };

    /** @brief A parameter of a method, constructor or free function. */
    struct MetaParam {
        // The parameter's own identifier, read from the declaration by the
        // walk. Falls back to "argN" for a parameter declared without a name,
        // and for the manifest's overload-selection path, which decomposes a
        // function type and so has no declaration to read names from.
        const char     *name = "";
        const TypeDesc *type = nullptr;
        bool            is_ref         = false;
        bool            is_mutable_ref = false;
        bool            is_out         = false; // manifest "out_params"; joins the return
    };

    /**
     * @brief The one function-pointer shape everything callable reduces to.
     *
     * `self` is the receiver for an instance method and default-constructed for
     * a static method or a free function. A plain function pointer, not
     * std::function: the emitted thunks are captureless lambdas, so the tables
     * stay trivially constructible.
     *
     * It takes the whole ObjectRef rather than a bare `void*` so a thunk can
     * reach `self.owner` — which is what lets a getter returning `T&` hand back
     * a handle that PINS its parent (borrow_any(td, child, self.owner)). With a
     * `void*` receiver that is structurally unexpressible, and the result is
     * the dangling sub-object handle that node's Wrap<T> and wasm's raw-pointer
     * returns have today (docs/MAIN-TODO.md §2).
     *
     * A null thunk means "described but not callable" — the type gate could not
     * marshal something. The member still appears in the metadata; invoking it
     * returns a Result carrying the reason.
     */
    using Thunk = Any (*)(const ObjectRef &self, const ArgList &args);

    struct MetaField {
        const char     *name = "";
        const TypeDesc *type = nullptr;
        const char     *doc  = "";

        bool      readonly = false;
        MetaRange range;

        const char *const *choices   = nullptr; // rosetta::combobox
        std::size_t        n_choices = 0;

        const MetaAnnotation *annotations   = nullptr;
        std::size_t           n_annotations = 0;

        // get(self, {}) -> value; set(self, {value}) -> none.
        // `set` is null for a readonly or unmarshalable field.
        Thunk get = nullptr;
        Thunk set = nullptr;
    };

    struct MetaMethod {
        const char     *name = "";
        const TypeDesc *ret  = nullptr;
        const char     *doc  = "";

        const MetaParam *params   = nullptr;
        std::size_t      n_params = 0;

        bool is_static  = false;
        bool is_const   = false;
        bool is_virtual = false;

        // Position within the same-named set, mirroring GenMethod. Unlike the
        // name-keyed backends, the dynamic model keeps every entry — these are
        // for diagnostics ("3 overloads of at(), none matched (...)").
        std::size_t overload_index = 0;
        std::size_t overload_count = 1;

        const MetaAnnotation *annotations   = nullptr;
        std::size_t           n_annotations = 0;

        Thunk       invoke      = nullptr;
        const char *skip_reason = ""; // non-empty exactly when invoke is null
    };

    struct MetaCtor {
        const MetaParam *params   = nullptr;
        std::size_t      n_params = 0;
        const char      *doc      = "";

        // Returns a NEW object, ownership transferred to the caller, or null on
        // failure. Paired with MetaClass::destroy.
        void *(*construct)(const ArgList &args) = nullptr;
    };

    struct MetaClass {
        const char *name      = ""; // unqualified, as bound (GenClass::expose)
        const char *qualified = ""; // "GEO::Mesh"
        const char *doc       = "";

        const MetaClass *const *bases   = nullptr;
        std::size_t             n_bases = 0;

        const MetaField  *fields    = nullptr;
        std::size_t       n_fields  = 0;
        const MetaMethod *methods   = nullptr;
        std::size_t       n_methods = 0;
        const MetaCtor   *ctors     = nullptr;
        std::size_t       n_ctors   = 0;

        bool is_abstract = false;

        const MetaAnnotation *annotations   = nullptr;
        std::size_t           n_annotations = 0;

        // Null when the destructor is not public (GenClass::is_destructible) —
        // such a class can still be borrowed and inspected, just never owned.
        void (*destroy)(void *) = nullptr;

        // The TypeDesc that names THIS class. Without it an Object could not be
        // turned back into a typed Any, so a handle could never be passed as an
        // argument to another method — `mesh.translate(vec)` would be
        // unexpressible. Set by the emitter; the two point at each other.
        const TypeDesc *self = nullptr;

        // ---- lookup (linear; see resolve() for the caching note) ----
        const MetaField  *field(std::string_view n) const;
        const MetaMethod *method(std::string_view n) const;    // first overload
        std::vector<const MetaMethod *> overloads(std::string_view n) const;
        bool                            derives_from(const MetaClass *b) const;
    };

    struct MetaEnum {
        const char           *name         = "";
        const char           *qualified    = "";
        const char           *underlying   = "int";
        const MetaEnumerator *values       = nullptr;
        std::size_t           n_values     = 0;
    };

    struct MetaFunction {
        const char      *name      = "";
        const char      *qualified = "";
        const char      *doc       = "";
        const TypeDesc  *ret       = nullptr;
        const MetaParam *params    = nullptr;
        std::size_t      n_params  = 0;

        std::size_t overload_index = 0;
        std::size_t overload_count = 1;

        Thunk       invoke      = nullptr;
        const char *skip_reason = "";
    };

    // ---------------------------------------------------------------------
    // Invocation
    // ---------------------------------------------------------------------

    /**
     * @brief Outcome of a dynamic call: a value or an explanation.
     *
     * Not an exception, because the primary consumers are language wrappers and
     * a C ABI, and unwinding through a foreign VM's frames is how you get a
     * crash instead of a TypeError. A wrapper turns a failed Result into its
     * host language's error in one place. The throwing forms below exist for
     * C++ callers who want them.
     */
    struct Result {
        Any         value;
        std::string error; // empty on success

        bool          ok() const { return error.empty(); }
        explicit      operator bool() const { return ok(); }
        const Any    &operator*() const { return value; }
    };

    /**
     * @brief How well `arg` fits `param`. Higher is better; `none` rejects.
     *
     * Deliberately a small ladder rather than C++'s full conversion lattice:
     * the arguments come from a dynamically typed host language, where the real
     * question is "did the user mean this overload", not "which standard
     * conversion sequence is shorter".
     */
    enum class Match : int {
        none    = -1, // incompatible; the candidate is out
        convert = 1,  // lossy or cross-kind but intended (number -> enum, number -> bool)
        promote = 2,  // same kind, different width (int argument -> double parameter)
        exact   = 3,  // same kind, same shape (or a derived class for an object)
    };

    Match match(const TypeDesc *param, const Any &arg);

    /**
     * @brief Pick the overload of `name` that best fits `args`.
     *
     * Sums match() over the arguments and takes the highest total; an equal
     * best total is reported as ambiguous rather than silently picking one.
     * Returns null on failure and fills `why` with a message naming every
     * candidate and why it lost — which is the error a script author needs and
     * the one the current name-keyed backends cannot produce at all, having
     * thrown the siblings away at generation time.
     *
     * PERFORMANCE. This is a linear scan plus a scoring pass per call. Fine for
     * menus, dialogs, REST routes and scripting glue; NOT fine in an inner loop.
     * The returned pointer is stable for the process, so a wrapper should cache
     * it per call site (Graphite's GOM does exactly this) and hot paths should
     * keep using an expanded binding. Sold honestly, "one lightweight wrapper"
     * is a statement about the wrapper's size, not about per-call cost.
     */
    const MetaMethod *resolve(const MetaClass &k, std::string_view name, const ArgList &args,
                              std::string *why = nullptr);

    // ---------------------------------------------------------------------
    // Object — the handle a host language holds
    // ---------------------------------------------------------------------

    /**
     * @brief An instance plus its class: get / set / call by name.
     *
     * This is the surface visitors/qml_reflected_object.h already exposes to
     * QML (getField / setField / callMethod over QVariant), generalized off Qt
     * and off any one Any type. Porting that file onto this class — and having
     * the QML inspector keep working while the backend shrinks — is the
     * cheapest available proof that the abstraction is the right one.
     */
    class Object {
    public:
        Object() = default;

        /** @brief Construct via the best-matching constructor. Owns the result. */
        static Result create(const MetaClass &k, const ArgList &args = {});

        /** @brief Wrap a pointer this handle does NOT own. */
        static Object borrow(const MetaClass &k, void *p);

        /** @brief Wrap a pointer whose lifetime is tied to `owner`. */
        static Object adopt(const MetaClass &k, void *p, std::shared_ptr<void> owner);

        bool             valid() const { return ref_.ptr && ref_.cls; }
        void            *ptr() const { return ref_.ptr; }
        const MetaClass *meta() const { return ref_.cls; }
        const ObjectRef &ref() const { return ref_; }

        Result get(std::string_view field) const;
        Result set(std::string_view field, const Any &v) const;
        Result call(std::string_view method, const ArgList &args = {}) const;

        /** @brief As an Any, sharing this handle's ownership. */
        Any as_any() const;

    private:
        ObjectRef ref_;
    };

    /** @brief Static-method and free-function calls (no receiver). */
    Result call_static(const MetaClass &k, std::string_view name, const ArgList &args = {});
    Result call_function(const MetaFunction &f, const ArgList &args = {});

    // ---------------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------------

    /**
     * @brief Everything the process knows how to reflect on.
     *
     * A generated module calls register_<lib>(registry()) once at load; a UI
     * then walks classes() to build its menus. Mutable by design — the
     * "define a class in Python, call it from Lua" story needs a MetaClass
     * whose thunks route back into a foreign VM, and that class has to be
     * addable at run time. Nothing here implements that yet; the registry just
     * refuses to be a compile-time-only structure so it does not have to be
     * rewritten when it lands.
     */
    class Registry {
    public:
        static Registry &instance();

        void add_class(const MetaClass *k);
        void add_enum(const MetaEnum *e);
        void add_function(const MetaFunction *f);

        // Accepts either the bound name ("Mesh") or the qualified one
        // ("GEO::Mesh"); qualified wins when both are registered.
        const MetaClass    *find_class(std::string_view n) const;
        const MetaEnum     *find_enum(std::string_view n) const;
        const MetaFunction *find_function(std::string_view n) const;

        std::vector<const MetaFunction *> function_overloads(std::string_view n) const;

        const std::vector<const MetaClass *>    &classes() const { return classes_; }
        const std::vector<const MetaEnum *>     &enums() const { return enums_; }
        const std::vector<const MetaFunction *> &functions() const { return functions_; }

        /** @brief Resolve every TypeDesc::cls back-pointer. Idempotent; the
         *  generated registration function calls it last. Two classes can name
         *  each other, so this cannot happen during static init. */
        void link();

    private:
        std::vector<const MetaClass *>    classes_;
        std::vector<const MetaEnum *>     enums_;
        std::vector<const MetaFunction *> functions_;
    };

    inline Registry &registry() { return Registry::instance(); }

} // namespace rosetta::dyn

// Templates the emitted thunks use to get in and out of Any. Separate header so
// the metadata surface above stays readable, and so a wrapper that only
// *consumes* metadata never instantiates them.
#include "inline/dynamic_cast.hxx"

#include "inline/dynamic.hxx"
