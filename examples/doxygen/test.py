#!/usr/bin/env python3
# Verifies that the documentation in polygon.h — which never mentions rosetta,
# carries no annotations, and was written as ordinary Doxygen — reached the
# compiled pybind11 module: as docstrings, as keyword arguments, and as real
# Python defaults.
#
#   build the module first:
#     ./generator out
#     cmake -S out/python -B out/python/build && cmake --build out/python/build
#   then:
#     python3 test.py

import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
for p in (os.path.join(here, "out", "python"),
          os.path.join(here, "out", "python", "build")):
    sys.path.insert(0, p)

import polygon

p = polygon.Polygon()
p.xs = [0.0, 4.0, 4.0, 0.0]
p.ys = [0.0, 0.0, 3.0, 3.0]

# ---- the class comment became the type's docstring -------------------------

assert polygon.Polygon.__doc__.startswith("A closed polygon"), polygon.Polygon.__doc__

# ---- `///` on a field became the property docstring ------------------------

assert polygon.Polygon.xs.__doc__ == "The x coordinate of each vertex, in order."
# ...including the trailing `///<` form
assert polygon.Polygon.ys.__doc__ == "The y coordinate of each vertex, in order."

# ---- @brief / @param / @return became the method docstring -----------------

doc = polygon.Polygon.resample.__doc__
assert "Resample the outline at evenly spaced points." in doc
assert "samples: how many points to produce" in doc
assert "closed: whether to repeat the first point at the end" in doc
assert "the coordinates, x and y interleaved" in doc

# @note and @warning are kept, labelled
simplify_doc = polygon.Polygon.simplify.__doc__
assert "Note: The polygon is modified in place." in simplify_doc
assert "Warning: Vertex indices are invalidated" in simplify_doc

# ---- parameter NAMES became keyword arguments ------------------------------

assert len(p.resample(samples=8, closed=False)) == 16
p.translate(dx=1.0, dy=2.0)
assert p.xs[0] == 1.0 and p.ys[0] == 2.0

# ---- default ARGUMENTS crossed the boundary --------------------------------

# `int samples = 64, bool closed = true` — calling with no arguments works, and
# the 65th point is the repeated first one.
assert len(p.resample()) == 2 * 65

# `const std::string &prefix = "polygon"` — a string default
assert p.describe() == "polygon with 4 vertices"
assert p.describe("quad") == "quad with 4 vertices"

# `Winding winding = Winding::CCW` — an enum default, which works because
# Winding is bound in this module too (see README).
assert p.area() == p.area(polygon.Winding.CCW)
assert p.area(polygon.Winding.CW) == -p.area(polygon.Winding.CCW)

# `double tolerance = 1e-6, int max_passes = 8` — floating point and int
assert p.simplify() is False

# ---- a default rosetta declined to repeat ----------------------------------

# `void set_epsilon(double eps = kDefaultEpsilon)`. The default is an
# UNQUALIFIED class constant, which would not resolve where the binding spells
# it, so the harvester refuses it: `eps` stays a required argument rather than
# becoming a wrong one. Passing it explicitly works, of course.
p.set_epsilon(1e-7)
try:
    p.set_epsilon()
    raise SystemExit("FAIL: 'eps' has no recoverable default and must be required")
except TypeError:
    pass

# ---- overloads keep their own documentation --------------------------------

# pybind11 dispatches on argument types, so both `scale` overloads are bound and
# each carries the text written above ITS declaration.
scale_doc = polygon.Polygon.scale.__doc__
assert "Scale the polygon uniformly about the origin." in scale_doc
assert "Scale the polygon independently on each axis." in scale_doc
p.scale(2.0)
p.scale(1.0, 0.5)

# ---- free functions too ----------------------------------------------------

d = polygon.centroid_distance.__doc__
assert "The distance between two polygons' centroids." in d
assert "a: the first polygon" in d
assert "the euclidean distance" in d
assert polygon.centroid_distance(a=p, b=p) == 0.0

# ---- and what must NOT be there --------------------------------------------

# `epsilon_` is private: never bound, never documented.
assert not hasattr(p, "epsilon_")

print("test.py OK — docstrings, keyword arguments and C++ defaults all came "
      "from the Doxygen comments in polygon.h")
