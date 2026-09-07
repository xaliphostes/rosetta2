# doxygen — the documentation your library already has

Your library is probably already documented. This example binds a header that
was written as ordinary, well-commented C++ — no annotations, no rosetta
include, no mention of rosetta anywhere — and shows that documentation coming
out the other side as Python docstrings, TSDoc, an OpenAPI spec and a Markdown
reference, along with the **parameter names** and **default arguments** the
declarations carry.

There is no manifest field to switch on: harvesting is the default. The only
setting is how to turn it **off** (`"doc_comments": false`).

## Files

| File | Role |
|---|---|
| `polygon.h` | the library — plain Doxygen, **never mentions rosetta** |
| `manifest.json` | names the classes and targets; nothing about documentation |
| `test.py` | asserts the docstrings, keyword arguments and C++ defaults reached the **Python** module |

## Run it

```bash
# (one-time) build the framework scaffolder -> <repo>/bin/rosetta_gen
cmake -S ../../tools/rosetta_gen -B ../../tools/rosetta_gen/build
cmake --build ../../tools/rosetta_gen/build

# 1. generate the driver project
../../bin/rosetta_gen manifest.json generated

# 2. build it -> the `generator` binary is dropped in this folder
cmake -S generated -B generated/build && cmake --build generated/build

# 3. run it -> emits the backend projects under out/
./generator out

# 4. read what came out
cat out/markdown/polygon.md
cat out/typescript/polygon.d.ts
```

## What the header says

```cpp
/**
 * @brief Resample the outline at evenly spaced points.
 *
 * @param samples how many points to produce
 * @param closed  whether to repeat the first point at the end
 * @return the coordinates, x and y interleaved
 */
std::vector<double> resample(int samples = 64, bool closed = true) const;
```

## What each backend does with it

**Markdown** (`out/markdown/polygon.md`) — the signature carries the names and
the defaults; the `@param` text becomes a list, `@return` its own line:

```markdown
### `resample(samples: int = 64, closed: bool = true) → double[]`

Resample the outline at evenly spaced points.

- `samples` — how many points to produce
- `closed` — whether to repeat the first point at the end

*Returns:* the coordinates, x and y interleaved
```

**TypeScript** (`out/typescript/polygon.d.ts`) — a TSDoc block, and a defaulted
parameter is declared *optional*, so an editor offers `resample()`:

```ts
/**
 * Resample the outline at evenly spaced points.
 * @param samples how many points to produce
 * @param closed whether to repeat the first point at the end
 * @returns the coordinates, x and y interleaved
 */
resample(samples?: number, closed?: boolean): number[];
```

**Python** (`out/python/auto_pybind.cpp`) — the names become real keyword
arguments and the defaults become real Python defaults:

```cpp
c.def("resample", &geom::Polygon::resample,
      "Resample the outline at evenly spaced points.\n\nArgs:\n    samples: …",
      py::arg("samples") = 64, py::arg("closed") = true);
```

```python
>>> help(polygon.Polygon.resample)
resample(self, samples: int = 64, closed: bool = True) -> list[float]

    Resample the outline at evenly spaced points.

    Args:
        samples: how many points to produce
        closed: whether to repeat the first point at the end

    Returns:
        the coordinates, x and y interleaved

>>> p.resample(samples=8, closed=False)      # keyword arguments
>>> p.resample()                             # the C++ defaults apply
```

**OpenAPI** (`out/openapi/openapi.json`) — the operation gets a description and
each positional argument gets its name, its text and its default:

```json
"description": "Resample the outline at evenly spaced points.\n\nReturns: …",
"prefixItems": [
  { "type": "integer", "title": "samples", "description": "how many points to produce", "default": 64 },
  { "type": "boolean", "title": "closed",  "description": "whether to repeat the first point at the end", "default": true }
]
```

## Test the Python binding

```bash
cmake -S out/python -B out/python/build && cmake --build out/python/build
python3 test.py
# -> test.py OK — docstrings, keyword arguments and C++ defaults all came from
#    the Doxygen comments in polygon.h
```

`test.py` is the interesting part of this example: it asserts, against the
compiled module, everything claimed above — and also the three cases where
rosetta deliberately does *less* than you might expect.

## The three deliberate limits, all visible in `polygon.h`

**1. A default rosetta will not repeat.**

```cpp
void set_epsilon(double eps = kDefaultEpsilon);   // kDefaultEpsilon is a private class constant
```

Reflection reports *that* `eps` has a default but not what it is — P2996 has no
way to ask — so the spelling is read from the header text instead. An
**unqualified** name like `kDefaultEpsilon` would not resolve where the binding
writes it, so the harvester refuses it: `eps` stays a required argument in
Python rather than becoming a wrong one. The Markdown and TypeScript output
still mark it optional (`eps?: number`), because that part *is* known.

The filter accepts literals (`64`, `1e-6`, `true`, `"polygon"`, `{}`) and
qualified-ids (`Winding::CCW`); it refuses calls, operators, commas and bare
identifiers. A wrong guess here would not produce a bad docstring — it would
produce a binding that does not compile.

**2. An enum default needs the enum bound.**

`area(Winding winding = Winding::CCW)` gets its default because `manifest.json`
lists `geom::Winding` as well as `geom::Polygon`. pybind converts an `arg`
default into a Python object at **module import**, so a default whose type is
not registered fails the import outright — drop `Winding` from the manifest and
the default is silently withheld rather than risking that. Defaults are emitted
for numbers, booleans, strings, and enums bound in the same module.

**3. Overloads follow each target's own rules.**

`scale(factor)` and `scale(sx, sy)` are matched to their own comments
independently, by arity — each pybind overload carries the text written above
its declaration. But the `.d.ts` declares only `scale(factor)`, because the
N-API module it describes binds methods by name and keeps the first; see
[`docs/COVERAGE.md`](../../docs/COVERAGE.md) and `out/coverage.json`. Where two
overloads cannot be told apart at all, **neither** is documented — a plausible
sentence on the wrong overload is worse than none.

## Precedence

Harvested text only ever fills a blank:

1. an inline `[[= rosetta::doc{"…"}]]` annotation — the harvester skips a member that carries one;
2. a JSON side-car entry (see [`annotate-manifest`](../annotate-manifest)), or a manifest `doc` on a free function;
3. the header's own comment.

So turning this on cannot change a binding you have already annotated, and the
two mechanisms compose: annotate the handful of members that need `range`,
`readonly` or a rewritten description, and let the headers document the rest.

## Notes

- Private members are never read — `Polygon::epsilon_` has a doc comment and is
  invisible to every backend, as it should be.
- `@tparam`, `@file`, `@ingroup` and the other bookkeeping commands are dropped;
  `@note`, `@warning`, `@deprecated`, `@throws`, `@pre`, `@post`, `@see` and
  `@since` are kept as labelled paragraphs.
- The harvester is a scanner, not a compiler: it does not expand macros or
  follow `#include`. Everything it reads is matched against the reflected
  signature before it is used, and dropped when the two disagree.
- Constructors are not documented (the IR has no doc slot for one), but their
  parameter *names* do reach `py::init` and the TypeScript `constructor(…)`.

See [`docs/MANIFEST.md`](../../docs/MANIFEST.md#doc-comments-doc_comments) for
the full reference.
