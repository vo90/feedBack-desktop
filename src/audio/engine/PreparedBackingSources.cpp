#include "PreparedBackingSources.h"
#include "BackingFrameMath.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace slopsmith {
namespace {
// Unlike AudioFormatReaderSource, propagate decode failures instead of quietly
// treating a damaged source as a successfully decoded silent stem.
class CheckedReaderSource final : public juce::AudioSource {
public:
    explicit CheckedReaderSource(std::unique_ptr<juce::AudioFormatReader> value)
        : reader(std::move(value)) {}
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        info.clearActiveBufferRegion();
        const auto remaining = std::max<juce::int64>(0, reader->lengthInSamples - position);
        const int frames = static_cast<int>(std::min<juce::int64>(remaining, info.numSamples));
        if (frames && !reader->read(info.buffer, info.startSample, frames, position, true, true))
            failed = true;
        position += frames;
    }
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::int64 position = 0;
    bool failed = false;
};
struct Source {
    std::unique_ptr<CheckedReaderSource> input;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
};
}

PreparedBackingSources::PreparedBackingSources(std::vector<juce::File> files,
    double outputSampleRate, double startSeconds, unsigned capacity, bool fullMixLast)
    : Thread("BackingDecode"), files_(std::move(files)), sampleRate_(outputSampleRate),
      requestedStart_(startSeconds), fullMixLast_(fullMixLast), queue_(static_cast<unsigned>(files_.size()), capacity) {
    if (!std::isfinite(sampleRate_) || sampleRate_ < 8000 || sampleRate_ > 384000
        || !std::isfinite(requestedStart_) || requestedStart_ < 0 || capacity < decodeBlock
        || (fullMixLast_ && files_.size() < 2))
        throw std::invalid_argument("Invalid backing decode format or start position");
    if (!startThread()) state_.store(State::failed, std::memory_order_release);
}

PreparedBackingSources::~PreparedBackingSources() {
    // Never kill a thread inside a codec. Cancellation is checked between every
    // source/chunk. Retirement may wait on I/O, so must NEVER run on audio/main UI
    // callbacks; the eventual session owner retires on its preparation worker.
    signalThreadShouldExit(); notify(); stopThread(-1);
}

PreparedBackingSources::ReadResult PreparedBackingSources::read(
    float* left, float* right, unsigned frames, const float* startGains, const float* endGains) noexcept {
    if (!left || !right || frames > queue_.capacity() || !startGains || !endGains)
        return {ReadStatus::failed, 0};
    std::fill_n(left, frames, 0.0f); std::fill_n(right, frames, 0.0f);
    for (unsigned i = 0; i < queue_.sourceCount(); ++i)
        if (!std::isfinite(startGains[i]) || !std::isfinite(endGains[i]))
            return {ReadStatus::failed, 0};
    const auto current = state();
    if (current == State::failed) return {ReadStatus::failed, 0};
    if (current != State::ready) return {ReadStatus::waiting, 0};
    if (!frames) return {ReadStatus::audio, 0};
    // Acquire EOF BEFORE the write frontier: observing EOF guarantees the final
    // write is visible. Reversing these reads can mistake pending data for EOF.
    const bool atEnd = decodedEnd_.load(std::memory_order_acquire);
    const unsigned available = queue_.availableToRead();
    if (available < frames && !atEnd) { ++underruns_; return {ReadStatus::waiting, 0}; }
    const unsigned count = std::min(frames, available);
    if (!count) return {ReadStatus::end, 0};
    if (!queue_.mix(left, right, count, startGains, endGains)) return {ReadStatus::failed, 0};
    return {ReadStatus::audio, count};
}

void PreparedBackingSources::run() {
    try {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::vector<Source> sources;
        double duration = 0;
        for (const auto& file : files_) {
            if (threadShouldExit()) return;
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if (!reader || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000
                || reader->sampleRate > 384000 || reader->lengthInSamples < 0
                || reader->numChannels == 0 || reader->numChannels > 2) {
                state_.store(State::failed, std::memory_order_release); return;
            }
            const double sourceDuration = reader->lengthInSamples / reader->sampleRate;
            if (fullMixLast_ && sources.size() + 1 == files_.size()) {
                if (std::abs(sourceDuration - duration) > std::max(.05, 2048.0 / sampleRate_)) {
                    state_.store(State::failed, std::memory_order_release); return;
                }
            } else duration = std::max(duration, sourceDuration);
            Source source;
            source.input = std::make_unique<CheckedReaderSource>(std::move(reader));
            source.resampler = std::make_unique<juce::ResamplingAudioSource>(source.input.get(), false, 2);
            source.resampler->setResamplingRatio(source.input->reader->sampleRate / sampleRate_);
            source.resampler->prepareToPlay(decodeBlock, sampleRate_);
            sources.push_back(std::move(source));
        }
        // A bounded queue, not a whole-song PCM allocation. Clamp before integer
        // conversion so corrupt duration metadata cannot overflow frame indices.
        constexpr double maxDuration = 24 * 60 * 60;
        if (duration <= 0 || duration > maxDuration) {
            state_.store(State::failed, std::memory_order_release); return;
        }
        const double start = std::min(requestedStart_, duration);
        const auto total = static_cast<std::uint64_t>(ceilFrameBoundary(duration * sampleRate_));
        const auto startFrame = start >= duration ? total
            : std::min(total, static_cast<std::uint64_t>(floorFrameBoundary(start * sampleRate_)));
        const double alignedStart = startFrame / sampleRate_;
        for (auto& source : sources) {
            source.input->position = std::min(source.input->reader->lengthInSamples,
                static_cast<juce::int64>(floorFrameBoundary(alignedStart * source.input->reader->sampleRate)));
            source.resampler->flushBuffers();
        }
        duration_.store(duration, std::memory_order_relaxed);
        startPosition_.store(std::min(alignedStart, duration), std::memory_order_relaxed);
        juce::AudioBuffer<float> decoded(static_cast<int>(sources.size()) * 2, decodeBlock);
        const auto remaining = total - startFrame;
        const auto prefill = std::min<std::uint64_t>(remaining, queue_.capacity() / 2);
        std::uint64_t produced = 0;
        while (produced < remaining && !threadShouldExit()) {
            const unsigned count = static_cast<unsigned>(std::min<std::uint64_t>(decodeBlock, remaining - produced));
            if (queue_.availableToWrite() < count) { wait(2); continue; }
            for (unsigned i = 0; i < sources.size(); ++i) {
                if (threadShouldExit()) return;
                float* channels[] = {decoded.getWritePointer(i * 2), decoded.getWritePointer(i * 2 + 1)};
                juce::AudioBuffer<float> buffer(channels, 2, count);
                sources[i].resampler->getNextAudioBlock({&buffer, 0, static_cast<int>(count)});
                if (sources[i].input->failed) {
                    state_.store(State::failed, std::memory_order_release); return;
                }
                for (const auto* channel : channels)
                    for (unsigned frame = 0; frame < count; ++frame)
                        if (!std::isfinite(channel[frame])) {
                            state_.store(State::failed, std::memory_order_release); return;
                        }
            }
            if (!queue_.write(decoded.getArrayOfReadPointers(), count)) {
                state_.store(State::failed, std::memory_order_release); return;
            }
            produced += count;
            if (produced >= prefill) state_.store(State::ready, std::memory_order_release);
        }
        if (!threadShouldExit()) {
            decodedEnd_.store(true, std::memory_order_release);
            state_.store(State::ready, std::memory_order_release);
        }
    } catch (...) {
        state_.store(State::failed, std::memory_order_release);
    }
}
} // namespace slopsmith
