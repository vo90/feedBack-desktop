#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace slopsmith {
// A bounded observation tap, never an audio transport. The callback only writes
// samples; copying and FFT work belong to consumers. Atomic cells make a racing
// snapshot well-defined in C++, with a sequence check rejecting torn windows.
class BackingAnalysis {
public:
    static constexpr unsigned capacity = 2048;
    struct Snapshot { std::array<float, capacity> samples{}; bool valid = false; };
    void append(const float* left, const float* right, unsigned frames) noexcept {
        sequence.fetch_add(1, std::memory_order_seq_cst);
        auto at = written.load(std::memory_order_seq_cst);
        for (unsigned i = 0; i < frames; ++i) cells[(at + i) % capacity].store((left[i] + right[i]) * .5f, std::memory_order_seq_cst);
        written.store(at + frames, std::memory_order_seq_cst);
        sequence.fetch_add(1, std::memory_order_seq_cst);
    }
    Snapshot read() const noexcept {
        Snapshot result;
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto before = sequence.load(std::memory_order_seq_cst);
            if (before & 1) continue;
            auto at = written.load(std::memory_order_seq_cst);
            for (unsigned i = 0; i < capacity; ++i)
                result.samples[i] = at + i < capacity ? 0 : cells[(at + i) % capacity].load(std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (before == sequence.load(std::memory_order_seq_cst)) { result.valid = at != 0; return result; }
        }
        return {};
    }
private:
    static_assert(std::atomic<float>::is_always_lock_free);
    std::array<std::atomic<float>, capacity> cells{};
    std::atomic<std::uint64_t> sequence{0}, written{0};
};
}
