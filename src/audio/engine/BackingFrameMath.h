#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace slopsmith {
// Duration metadata is seconds; converting integer frames to seconds and back
// can land a few ulps above an integer. Do not add a spurious sample at EOF or
// lose one at a seek boundary. Callers validate finite, bounded nonnegative input.
inline double snapFrameBoundary(double frames) noexcept {
    const double nearest = std::round(frames);
    const double tolerance = 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(frames));
    return std::abs(frames - nearest) <= tolerance ? nearest : frames;
}
inline double ceilFrameBoundary(double frames) noexcept { return std::ceil(snapFrameBoundary(frames)); }
inline double floorFrameBoundary(double frames) noexcept { return std::floor(snapFrameBoundary(frames)); }
}
