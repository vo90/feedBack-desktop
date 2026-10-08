#pragma once

#include <atomic>
#include <cstdint>

namespace slopsmith {

// Observation only: none of these fields changes the legacy position/scoring
// contract. A render boundary is not a measured DAC presentation timestamp.
struct BackingRenderObservation {
    bool valid = false;
    std::uint64_t routeGeneration = 0;
    double startedAtMs = 0, sourcePositionAfterRender = 0, sampleRate = 0;
    double firstFramePosition = 0;
    int frames = 0, outputLatencyFrames = -1;
    int stretchInputLatencyFrames = 0, stretchOutputLatencyFrames = 0;
};

struct BackingClockSample {
    bool valid = false;
    double position = 0, sampledAtMs = 0, rate = 1;
    std::uint64_t sequence = 0, generation = 0;
    bool playing = false, ended = false, failed = false;
    BackingRenderObservation render;
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
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    void publish(double position, double sampledAtMs, double rate,
                 std::uint64_t generation, bool playing, bool ended,
                 const BackingRenderObservation& render = {}, bool failed = false) noexcept {
        const auto version = sequence_.load();
        sequence_.store(version + 1);
        position_.store(position); sampledAtMs_.store(sampledAtMs); rate_.store(rate);
        generation_.store(generation); flags_.store((playing ? 1u : 0u) | (ended ? 2u : 0u) | (failed ? 4u : 0u));
        renderValid_.store(render.valid); routeGeneration_.store(render.routeGeneration);
        renderStarted_.store(render.startedAtMs); sourcePosition_.store(render.sourcePositionAfterRender);
        firstFramePosition_.store(render.firstFramePosition);
        sampleRate_.store(render.sampleRate); renderFrames_.store(render.frames);
        outputLatency_.store(render.outputLatencyFrames);
        stretchInputLatency_.store(render.stretchInputLatencyFrames);
        stretchOutputLatency_.store(render.stretchOutputLatencyFrames);
        sequence_.store(version + 2);
    }

    BackingClockSample read() const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = sequence_.load();
            if (before == 0 || (before & 1)) continue;
            BackingClockSample s;
            s.position = position_.load(); s.sampledAtMs = sampledAtMs_.load(); s.rate = rate_.load();
            s.generation = generation_.load(); const auto flags = flags_.load();
            s.render.valid = renderValid_.load(); s.render.routeGeneration = routeGeneration_.load();
            s.render.startedAtMs = renderStarted_.load(); s.render.sourcePositionAfterRender = sourcePosition_.load();
            s.render.sampleRate = sampleRate_.load(); s.render.frames = renderFrames_.load();
            s.render.firstFramePosition = firstFramePosition_.load();
            s.render.outputLatencyFrames = outputLatency_.load();
            s.render.stretchInputLatencyFrames = stretchInputLatency_.load();
            s.render.stretchOutputLatencyFrames = stretchOutputLatency_.load();
            if (before != sequence_.load()) continue;
            s.valid = true; s.sequence = before / 2;
            s.playing = (flags & 1) != 0; s.ended = (flags & 2) != 0;
            s.failed = (flags & 4) != 0;
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
    std::atomic<bool> renderValid_{false};
    std::atomic<std::uint64_t> routeGeneration_{0};
    std::atomic<double> renderStarted_{0}, sourcePosition_{0}, sampleRate_{0};
    std::atomic<double> firstFramePosition_{0};
    std::atomic<int> renderFrames_{0}, outputLatency_{-1}, stretchInputLatency_{0}, stretchOutputLatency_{0};
};
} // namespace slopsmith
