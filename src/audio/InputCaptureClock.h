#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>

// Timestamp the end of the pre-gate sample ring in the native monotonic domain.
// This is callback receipt time, not an ADC timestamp. Driver/analysis latency
// remains the separately calibrated input correction.
class InputCaptureClock {
public:
    void publish(std::uint64_t index, double atMs) noexcept {
        sequence.fetch_add(1); frame.store(index); time.store(atMs); sequence.fetch_add(1);
    }
    std::optional<double> timeFor(std::uint64_t index, double sampleRate) const noexcept {
        if (!std::isfinite(sampleRate) || sampleRate <= 0) return {};
        for (int i = 0; i < 3; ++i) {
            const auto before = sequence.load();
            if (!before || (before & 1)) continue;
            const auto last = frame.load(); const auto at = time.load();
            if (before != sequence.load()) continue;
            if (!last || index > last || last - index > sampleRate * 2) return {};
            return at - (last - index) * 1000.0 / sampleRate;
        }
        return {};
    }
private:
    static_assert(std::atomic<double>::is_always_lock_free);
    std::atomic<std::uint64_t> sequence{0}, frame{0};
    std::atomic<double> time{0};
};
