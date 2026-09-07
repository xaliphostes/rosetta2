// Copyright (c) fmaerten@gmail.com
// License: MIT

// An ordinary, well-documented C++ header. It does not include rosetta, does
// not mention rosetta, and carries no annotations — it is written exactly as it
// would be if bindings were never going to exist.
//
// Everything the generated bindings say about this API is read from the
// comments and declarations below.
#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace geom {

    /// The direction a polygon's vertices are wound in.
    enum class Winding {
        CCW, ///< Counter-clockwise: a positive signed area.
        CW,  ///< Clockwise: a negative signed area.
    };

    /**
     * @brief A closed polygon, stored as parallel coordinate arrays.
     *
     * The first vertex is not repeated at the end; the closing edge from the
     * last vertex back to the first is implied.
     */
    class Polygon {
    public:
        /// The x coordinate of each vertex, in order.
        std::vector<double> xs;
        std::vector<double> ys; ///< The y coordinate of each vertex, in order.

        /**
         * @brief The signed area enclosed by the polygon.
         *
         * @param winding which direction to treat as positive
         * @return the area; negative when the polygon is wound the other way
         */
        double area(Winding winding = Winding::CCW) const {
            double sum = 0;
            for (std::size_t i = 0; i < xs.size(); ++i) {
                const std::size_t j = (i + 1) % xs.size();
                sum += xs[i] * ys[j] - xs[j] * ys[i];
            }
            sum *= 0.5;
            return winding == Winding::CCW ? sum : -sum;
        }

        /**
         * @brief Resample the outline at evenly spaced points.
         *
         * @param samples how many points to produce
         * @param closed  whether to repeat the first point at the end
         * @return the coordinates, x and y interleaved
         */
        std::vector<double> resample(int samples = 64, bool closed = true) const {
            std::vector<double> out;
            for (int i = 0; i < samples + (closed ? 1 : 0); ++i) {
                const std::size_t k = xs.empty() ? 0 : (std::size_t)i % xs.size();
                out.push_back(xs.empty() ? 0.0 : xs[k]);
                out.push_back(ys.empty() ? 0.0 : ys[k]);
            }
            return out;
        }

        /// Move every vertex by the given offset.
        /// @param dx how far to move along x
        /// @param dy how far to move along y
        void translate(double dx, double dy) {
            for (std::size_t i = 0; i < xs.size(); ++i) {
                xs[i] += dx;
                ys[i] += dy;
            }
        }

        /// Scale the polygon uniformly about the origin.
        /// @param factor the scale factor applied to both axes
        void scale(double factor) { scale(factor, factor); }

        /// Scale the polygon independently on each axis.
        /// @param sx the scale factor along x
        /// @param sy the scale factor along y
        void scale(double sx, double sy) {
            for (std::size_t i = 0; i < xs.size(); ++i) {
                xs[i] *= sx;
                ys[i] *= sy;
            }
        }

        /**
         * @brief Drop vertices that lie within `tolerance` of their neighbour.
         *
         * @param tolerance  the distance below which two vertices are the same
         * @param max_passes how many times to sweep the vertex list
         * @return whether anything was removed
         *
         * @note The polygon is modified in place.
         * @warning Vertex indices are invalidated when this returns true.
         */
        bool simplify(double tolerance = 1e-6, int max_passes = 8) {
            bool removed = false;
            for (int pass = 0; pass < max_passes; ++pass) {
                for (std::size_t i = xs.size(); i-- > 1;) {
                    if (std::abs(xs[i] - xs[i - 1]) < tolerance &&
                        std::abs(ys[i] - ys[i - 1]) < tolerance) {
                        xs.erase(xs.begin() + (long)i);
                        ys.erase(ys.begin() + (long)i);
                        removed = true;
                    }
                }
            }
            return removed;
        }

        /// A one-line summary of the polygon.
        /// @param prefix text placed before the vertex count
        /// @return the assembled description
        std::string describe(const std::string &prefix = "polygon") const {
            return prefix + " with " + std::to_string(xs.size()) + " vertices";
        }

        /// Set the tolerance used by future simplify() calls.
        /// @param eps the new tolerance
        void set_epsilon(double eps = kDefaultEpsilon) { epsilon_ = eps; }

        /// The number of vertices.
        /// @deprecated Use `len(polygon.xs)` instead.
        std::size_t count() const { return xs.size(); }

    private:
        /// Scratch state — never part of the binding, and never documented in one.
        double epsilon_ = kDefaultEpsilon;

        static constexpr double kDefaultEpsilon = 1e-9;
    };

    /**
     * @brief The distance between two polygons' centroids.
     *
     * @param a the first polygon
     * @param b the second polygon
     * @return the euclidean distance
     */
    inline double centroid_distance(const Polygon &a, const Polygon &b) {
        auto mean = [](const std::vector<double> &v) {
            double s = 0;
            for (double x : v) {
                s += x;
            }
            return v.empty() ? 0.0 : s / (double)v.size();
        };
        const double dx = mean(a.xs) - mean(b.xs);
        const double dy = mean(a.ys) - mean(b.ys);
        return std::sqrt(dx * dx + dy * dy);
    }

} // namespace geom
