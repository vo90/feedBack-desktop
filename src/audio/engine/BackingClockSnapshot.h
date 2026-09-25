#pragma once

#include <atomic>
#include <cstdint>

namespace slopsmith {

struct BackingClockSample {
    bool valid = false;
    double position = 0, sampledAtMs = 0, rate = 1;
    std::uint64_t sequence = 0, generation = 0;
    bool playing = false, ended = false;
};

// Publishers MUST already own BackingPlayer's existing lock. Its control
// methods hold that lock; audio callbacks only publish after their existing
// try-lock succeeded. No new lock, allocation or retry is added to the writer.
// Atomic payload fields avoid the data race of a plain-struct seqlock. Using
// sequential consistency throughout makes the accepted even-version snapshot
// coherent across fields on every supported architecture.
class BackingClockSnapshot {
public:
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    void publish(double position, double sampledAtMs, double rate,
                 std::uint64_t generation, bool playing, bool ended) noexcept {
        const auto version = sequence_.load();
        sequence_.store(version + 1);
        position_.store(position); sampledAtMs_.store(sampledAtMs); rate_.store(rate);
        generation_.store(generation); flags_.store((playing ? 1u : 0u) | (ended ? 2u : 0u));
        sequence_.store(version + 2);
    }

    BackingClockSample read() const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = sequence_.load();
            if (before == 0 || (before & 1)) continue;
            BackingClockSample s;
            s.position = position_.load(); s.sampledAtMs = sampledAtMs_.load(); s.rate = rate_.load();
            s.generation = generation_.load(); const auto flags = flags_.load();
            if (before != sequence_.load()) continue;
            s.valid = true; s.sequence = before / 2;
            s.playing = (flags & 1) != 0; s.ended = (flags & 2) != 0;
            return s;
        }
        // The caller discards an unavailable observation; never spin waiting
        // for audio or synthesize a fresh timestamp for an old position.
        return {};
    }

private:
    std::atomic<std::uint64_t> sequence_{0}, generation_{0};
    std::atomic<double> position_{0}, sampledAtMs_{0}, rate_{1};
    std::atomic<std::uint64_t> flags_{0};
};
} // namespace slopsmith
