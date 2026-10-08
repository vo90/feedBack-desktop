#pragma once
#include "PreparedBackingSources.h"
#include "../BackingLeveler.h"
#include "signalsmith-stretch.h"
#include <array>

namespace slopsmith {
// Prepared off the callback. After priming, render/gain state has one audio
// consumer. The owner swaps and retires complete sessions, never their buffers.
class BackingSession {
public:
    using Gains = std::array<float, BackingSourceQueue::maxSources>;
    enum class Result { audio, waiting, ended, failed };
    BackingSession(const std::vector<juce::File>& files, double sampleRate, int blockSize,
                   double position, double rate, const Gains& gains, bool fullMixLast = false);
    Result prime(); // preparation worker; call until audio/failed
    Result render(int frames, const Gains& gains); // callback; bounded, no I/O
    const juce::AudioBuffer<float>& buffer() const { return output; }
    double position() const { return primeState == 2 && rendered >= totalOutput ? duration() : std::min(duration(), start + rendered * rate / sr); }
    double duration() const { return sources->duration(); }
    double sourcePosition() const { return sources->startPosition() + sources->consumedFrames() / sr; }
    double sampleRate() const { return sr; }
    double playbackRate() const { return rate; }
    int blockSize() const { return block; }
    unsigned sourceCount() const { return sources->sourceCount(); }
    int inputLatency() const { return bypass ? 0 : stretch.inputLatency(); }
    int outputLatency() const { return bypass ? 0 : stretch.outputLatency(); }
    std::uint64_t renderedFrames() const { return rendered; }
    void retainLeveler(const BackingSession& previous) { leveler = previous.leveler; }
private:
    Result process(int frames, const Gains& gains, bool discard);
    const double sr, rate;
    const int block;
    const bool bypass;
    double start = 0, fraction = 0;
    std::uint64_t rendered = 0, totalOutput = 0;
    int primeState = 0, discarded = 0;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::unique_ptr<PreparedBackingSources> sources;
    juce::AudioBuffer<float> input, output;
    Gains currentGains;
    BackingLeveler leveler;
};
}
