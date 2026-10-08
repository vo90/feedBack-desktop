#pragma once

#include "BackingSourceQueue.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

namespace slopsmith {

// A single prepared decoder generation. Worker-owned file I/O/resampling feeds
// a bounded queue. The audio thread can only consume a coherent group of source
// frames. Seek/format changes create another generation; they never reset this
// queue under a live consumer. Ownership/swap/retirement belongs to BackingPlayer.
class PreparedBackingSources final : private juce::Thread {
public:
    enum class State { preparing, ready, failed };
    enum class ReadStatus { audio, waiting, end, failed };
    static_assert(std::atomic<State>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    struct ReadResult { ReadStatus status; unsigned frames; };
    static constexpr unsigned decodeBlock = 1024;

    PreparedBackingSources(std::vector<juce::File> files, double outputSampleRate,
                           double startSeconds = 0,
                           unsigned capacity = BackingSourceQueue::defaultCapacity, bool fullMixLast = false);
    ~PreparedBackingSources() override; // preparation/retirement worker; may wait on I/O

    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    double duration() const noexcept { return duration_.load(std::memory_order_relaxed); }
    double startPosition() const noexcept { return startPosition_.load(std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_; }
    unsigned sourceCount() const noexcept { return queue_.sourceCount(); }
    std::uint64_t consumedFrames() const noexcept { return queue_.consumedFrames(); }
    std::uint64_t underruns() const noexcept { return underruns_; } // consumer only

    // Audio consumer only; never waits or allocates. Short final reads are padded
    // with silence; waiting/failure clears the whole block and consumes nothing.
    // Gains have one value per source. Caller owns the destination capacity.
    ReadResult read(float* left, float* right, unsigned frames,
                    const float* startGains, const float* endGains) noexcept;

private:
    void run() override;
    const std::vector<juce::File> files_;
    const double sampleRate_, requestedStart_;
    const bool fullMixLast_;
    BackingSourceQueue queue_;
    std::atomic<State> state_{State::preparing};
    std::atomic<bool> decodedEnd_{false};
    std::atomic<double> duration_{0}, startPosition_{0};
    std::uint64_t underruns_ = 0;
};
} // namespace slopsmith
