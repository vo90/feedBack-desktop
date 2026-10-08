#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace slopsmith {

// One producer publishes ALL sources at one frame frontier. One audio consumer
// mixes them before releasing space. A slow decoder therefore cannot move one
// stem ahead of another. Allocate on the control thread; never reset a live queue.
class BackingSourceQueue {
public:
    static constexpr unsigned maxSources = 32; // includes an optional full mix
    static constexpr unsigned defaultCapacity = 32768;
    static constexpr unsigned maxCapacity = 262144;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    BackingSourceQueue(unsigned sources, unsigned capacity = defaultCapacity)
        : sources_(sources), capacity_(capacity), samples_(storageSize(sources, capacity)) {}

    unsigned sourceCount() const noexcept { return sources_; }
    unsigned capacity() const noexcept { return capacity_; }

    // Producer only. Channels are planar [source0 L, source0 R, source1 L, ...].
    bool write(const float* const* channels, unsigned frames) noexcept {
        const auto write = written_.load(std::memory_order_relaxed);
        const auto read = consumed_.load(std::memory_order_acquire);
        if (frames > capacity_ || write - read + frames > capacity_ || !channels) return false;
        for (unsigned c = 0; c < sources_ * 2; ++c) if (!channels[c]) return false;
        const unsigned offset = static_cast<unsigned>(write % capacity_);
        const unsigned first = std::min(frames, capacity_ - offset);
        for (unsigned c = 0; c < sources_ * 2; ++c) {
            auto* destination = samples_.data() + c * capacity_;
            std::copy_n(channels[c], first, destination + offset);
            std::copy_n(channels[c] + first, frames - first, destination);
        }
        written_.store(write + frames, std::memory_order_release);
        return true;
    }

    // Consumer only. All-or-nothing: starvation leaves destinations and the
    // shared read frontier untouched. Gains are snapshotted by the owner before
    // calling. Gain interpolation reaches endGains on the final frame, including
    // when a block wraps in the ring. No clipping/normalization per source.
    bool mix(float* left, float* right, unsigned frames,
             const float* startGains, const float* endGains) noexcept {
        const auto read = consumed_.load(std::memory_order_relaxed);
        const auto write = written_.load(std::memory_order_acquire);
        if (frames > capacity_ || frames > write - read || !left || !right
            || !startGains || !endGains) return false;
        for (unsigned frame = 0; frame < frames; ++frame) {
            const auto offset = static_cast<unsigned>((read + frame) % capacity_);
            const float progress = static_cast<float>(frame + 1) / static_cast<float>(frames);
            float l = 0, r = 0;
            for (unsigned source = 0; source < sources_; ++source) {
                const float gain = startGains[source] + (endGains[source] - startGains[source]) * progress;
                l += samples_[(source * 2) * capacity_ + offset] * gain;
                r += samples_[(source * 2 + 1) * capacity_ + offset] * gain;
            }
            left[frame] = l; right[frame] = r;
        }
        consumed_.store(read + frames, std::memory_order_release);
        return true;
    }

    // These accessors have a designated thread, not a third-thread diagnostic
    // contract. A diagnostic snapshot must use the owner's published counters.
    unsigned availableToRead() const noexcept { // consumer
        const auto read = consumed_.load(std::memory_order_relaxed);
        return static_cast<unsigned>(written_.load(std::memory_order_acquire) - read);
    }
    unsigned availableToWrite() const noexcept { // producer
        const auto write = written_.load(std::memory_order_relaxed);
        return capacity_ - static_cast<unsigned>(write - consumed_.load(std::memory_order_acquire));
    }
    std::uint64_t consumedFrames() const noexcept { return consumed_.load(std::memory_order_relaxed); }

private:
    static std::size_t storageSize(unsigned sources, unsigned capacity) {
        if (sources == 0 || sources > maxSources || capacity == 0 || capacity > maxCapacity)
            throw std::invalid_argument("Backing source queue exceeds its bounded dimensions");
        return std::size_t(sources) * 2 * capacity;
    }
    const unsigned sources_, capacity_;
    std::vector<float> samples_;
    alignas(64) std::atomic<std::uint64_t> written_{0};
    alignas(64) std::atomic<std::uint64_t> consumed_{0};
};
} // namespace slopsmith
