#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace slopsmith {
// Setup-only, pre-gain metering. The owner serializes prepare/read/arm against
// device lifecycle changes. prepare runs with the audio callback stopped.
// process never allocates, locks, or changes the input used by detection/DSP.
class InputChannelMeters {
    struct Channel {
        std::atomic<float> peak {0}, rms {0};
        float heldPeak = 0;
    };
    std::unique_ptr<Channel[]> channels;
    int count = 0;
    double sampleRate = 48000;
    std::atomic<int> framesRemaining {0};
    std::atomic<uint64_t> sequence {0};
public:
    struct Level { float peak, rms; };
    void prepare(int n, double sr) {
        framesRemaining.store(0);
        count = std::max(0, n);
        sampleRate = sr > 0 ? sr : 48000;
        channels = std::make_unique<Channel[]>(count);
        sequence.store(0);
    }
    void arm() { framesRemaining.store(static_cast<int>(sampleRate * 2)); }
    uint64_t getSequence() const { return sequence.load(std::memory_order_acquire); }
    std::vector<Level> read() const {
        std::vector<Level> result;
        result.reserve(count);
        for (int c = 0; c < count; ++c)
            result.push_back({channels[c].peak.load(), channels[c].rms.load()});
        return result;
    }
    void process(const float* const* input, int n, int frames) {
        if (frames <= 0 || framesRemaining.load(std::memory_order_relaxed) <= 0) return;
        framesRemaining.fetch_sub(frames, std::memory_order_relaxed);
        const float decay = static_cast<float>(std::exp(-frames / (sampleRate * 0.15)));
        for (int c = 0; c < count; ++c) {
            float peak = 0;
            double energy = 0;
            const auto* data = input && c < n ? input[c] : nullptr;
            if (data) for (int i = 0; i < frames; ++i) {
                const float x = std::isfinite(data[i]) ? data[i] : 0;
                peak = std::max(peak, std::abs(x));
                energy += static_cast<double>(x) * x;
            }
            channels[c].heldPeak = std::max(peak, channels[c].heldPeak * decay);
            channels[c].peak.store(channels[c].heldPeak, std::memory_order_relaxed);
            channels[c].rms.store(static_cast<float>(std::sqrt(energy / frames)), std::memory_order_relaxed);
        }
        sequence.fetch_add(1, std::memory_order_release);
    }
};
}
