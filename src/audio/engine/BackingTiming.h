#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace slopsmith {

// Timing contract for the unified backing session. All timestamps are in ONE
// monotonic domain; bridges must map clock domains before constructing an anchor.
// This helper does not sleep, buffer input, read devices, or change calibration.
// It is initially tested independently, before migrating legacy clock consumers.
struct BackingPresentationAnchor {
    double firstFrameSongSeconds = 0; // includes DSP compensation, not A/V trim
    double callbackAtMs = 0;         // callback entry, NOT end-of-render timestamp
    double outputLatencyMs = 0;      // callback -> first frame, driver estimate
    double rate = 1;
    std::uint64_t session = 0, route = 0;
};

inline std::optional<double> backingSongTimeAt(
    const BackingPresentationAnchor& anchor, double eventAtMs,
    double residualAvMs, std::uint64_t session, std::uint64_t route) noexcept {
    if (session != anchor.session || route != anchor.route
        || !std::isfinite(anchor.firstFrameSongSeconds) || !std::isfinite(anchor.callbackAtMs)
        || !std::isfinite(anchor.outputLatencyMs) || anchor.outputLatencyMs < 0
        || !std::isfinite(anchor.rate) || anchor.rate <= 0
        || !std::isfinite(eventAtMs) || !std::isfinite(residualAvMs)) return std::nullopt;
    // Negative residual A/V moves the chart/judgment reference earlier.
    // Do not clamp pre-roll to zero: it describes frames not yet presented and
    // clamping it would make early notes/scoring inconsistent at song starts.
    const double result = anchor.firstFrameSongSeconds
        + (eventAtMs - anchor.callbackAtMs - anchor.outputLatencyMs + residualAvMs)
            * anchor.rate / 1000.0;
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

inline std::optional<double> backingSongTimeForDetection(
    const BackingPresentationAnchor& anchor, double detectedAtMs,
    double captureAndDetectionAgeMs, double residualAvMs,
    std::uint64_t session, std::uint64_t route) noexcept {
    if (!std::isfinite(captureAndDetectionAgeMs) || captureAndDetectionAgeMs < 0)
        return std::nullopt;
    return backingSongTimeAt(anchor, detectedAtMs - captureAndDetectionAgeMs,
                             residualAvMs, session, route);
}
} // namespace slopsmith
