#pragma once
#include "EngineState.h"
#include "BackingClockSnapshot.h"
#include "BackingSession.h"
#include "BackingAnalysis.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace slopsmith {
// One preparation owner and one callback consumer. Control mutations are brief;
// decoding, priming, and retiring sessions happen on the preparation thread.
// Existing engine callbacks retain their non-blocking try-lock mix boundary.
class BackingPlayer final : private juce::Thread {
public:
    static constexpr double kMaxSpeed = 4.0, kSpeedBypassEpsilon = 1e-4;
    using Gains = BackingSession::Gains;
    explicit BackingPlayer(EngineState&);
    ~BackingPlayer() override;

    // Submit on the caller thread to preserve invocation order. Wait only from
    // an async/control worker, never Electron's main thread or an audio callback.
    std::uint64_t beginLoad(const std::vector<juce::File>&, const std::vector<float>&, bool fullMixLast = false);
    std::uint64_t beginSeek(double);
    std::uint64_t beginRate(double);
    bool waitForRequest(std::uint64_t);
    std::uint64_t currentRequestId() const { return requested.load(); }
    bool setSourceGains(const std::vector<float>&);
    bool load(const juce::File& file) { return waitForRequest(beginLoad({file}, {1})); }
    void setPosition(double value) { waitForRequest(beginSeek(value)); }
    void setSpeed(double value) { waitForRequest(beginRate(value)); }
    void start();
    void stop();
    void prepare(double sr, int bs, int reportedOutputLatencyFrames = -1);
    void invalidateOutputTiming();

    bool isPlaying() const { return playing.load(); }
    double getPosition() const { return cachedPosition.load(); }
    double getDuration() const { return cachedDuration.load(); }
    BackingClockSample getClockSnapshot() const { return clockSnapshot.read(); }
    BackingAnalysis::Snapshot getAnalysis() const { const auto clock = clockSnapshot.read(); return clock.playing && clock.render.valid ? analysis.read() : BackingAnalysis::Snapshot{}; }
    juce::CriticalSection& getLock() { return lock; }
    bool readyLocked() const { return active && committed == requested.load() && playing.load(); }
    int renderBlockLocked(int numSamples, double callbackAtMs = 0);
    const juce::AudioBuffer<float>& renderBuffer() const { return active->buffer(); }

private:
    void run() override;
    std::uint64_t scheduleLocked(double position, bool newSong);
    void publishClockLocked();
    juce::CriticalSection lock;
    std::unique_ptr<BackingSession> active;
    std::vector<juce::File> files;
    Gains gains{};
    double requestedPosition = 0, requestedRate = 1, outputRate = 48000;
    int outputBlock = 256;
    bool wantsPlay = false, outputReady = true, newSong = true, ended = false, succeeded = false;
    std::atomic<bool> playing{false};
    std::atomic<double> cachedPosition{0}, cachedDuration{0};
    std::atomic<std::uint64_t> requested{0}, completed{0};
    std::uint64_t committed = 0, clockGeneration = 0, songSerial = 0, committedSong = 0;
    bool workerStarted = false;
    bool fullMixLast = false;
    bool buffering = false;
    bool failed = false;
    double drainUntilMs = 0;
    BackingClockSnapshot clockSnapshot;
    BackingRenderObservation renderObservation;
    BackingAnalysis analysis;
};
}
