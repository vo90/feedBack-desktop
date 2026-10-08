#include "BackingPlayer.h"
#include <cmath>

namespace slopsmith {
BackingPlayer::BackingPlayer(EngineState& state) : Thread("BackingPreparation") {
    outputRate = state.currentSampleRate.load(); outputBlock = state.outputBlockSize.load();
    // Loading before a device opens may use the default preparation format.
    if (!std::isfinite(outputRate) || outputRate < 8000 || outputRate > 384000) outputRate = 48000;
    if (outputBlock <= 0 || outputBlock > 16384) outputBlock = 256;
    workerStarted = startThread();
}
BackingPlayer::~BackingPlayer() { signalThreadShouldExit(); notify(); stopThread(-1); }

void BackingPlayer::publishClockLocked() {
    clockSnapshot.publish(cachedPosition.load(), juce::Time::getMillisecondCounterHiRes(),
        requestedRate, clockGeneration, playing.load() && !buffering, ended, renderObservation, failed);
}

std::uint64_t BackingPlayer::scheduleLocked(double position, bool isNewSong) {
    requestedPosition = position; newSong = isNewSong;
    playing.store(false); ended = false; succeeded = false; failed = false; buffering = false; drainUntilMs = 0;
    cachedPosition.store(position); renderObservation.valid = false;
    ++clockGeneration;
    const auto id = requested.fetch_add(1) + 1;
    publishClockLocked(); notify();
    return id;
}

std::uint64_t BackingPlayer::beginLoad(const std::vector<juce::File>& paths, const std::vector<float>& values, bool fullLast) {
    if (paths.empty() || paths.size() > BackingSourceQueue::maxSources || paths.size() != values.size()) return 0;
    for (auto value : values) if (!std::isfinite(value) || value < 0 || value > 2) return 0;
    const juce::ScopedLock owner(lock);
    files = paths; gains.fill(0); std::copy(values.begin(), values.end(), gains.begin());
    fullMixLast = fullLast;
    wantsPlay = false; requestedRate = 1; cachedDuration.store(0); ++songSerial;
    return scheduleLocked(0, true);
}

std::uint64_t BackingPlayer::beginSeek(double seconds) {
    if (!std::isfinite(seconds)) return 0;
    const juce::ScopedLock owner(lock);
    if (files.empty()) return 0;
    return scheduleLocked(std::clamp(seconds, 0.0, cachedDuration.load()), false);
}

std::uint64_t BackingPlayer::beginRate(double rate) {
    if (!std::isfinite(rate) || rate <= 0) return 0;
    const juce::ScopedLock owner(lock);
    rate = std::clamp(rate, .01, kMaxSpeed);
    if (std::abs(rate - 1) < kSpeedBypassEpsilon) rate = 1;
    if (rate == requestedRate) return requested.load();
    requestedRate = rate;
    if (files.empty()) return 0;
    return scheduleLocked(cachedPosition.load(), false);
}

bool BackingPlayer::waitForRequest(std::uint64_t id) {
    if (!id || !workerStarted) return false;
    while (!threadShouldExit() && requested.load() == id && completed.load() < id) juce::Thread::sleep(1);
    const juce::ScopedLock owner(lock);
    return requested.load() == id && completed.load() == id && succeeded;
}

bool BackingPlayer::setSourceGains(const std::vector<float>& values) {
    for (float value : values) if (!std::isfinite(value) || value < 0 || value > 2) return false;
    const juce::ScopedLock owner(lock);
    if (values.size() != files.size()) return false;
    std::copy(values.begin(), values.end(), gains.begin());
    return true;
}

void BackingPlayer::start() {
    const juce::ScopedLock owner(lock);
    wantsPlay = !files.empty(); ended = false; buffering = true;
    playing.store(wantsPlay && outputReady && active && committed == requested.load());
    ++clockGeneration; renderObservation.valid = false; publishClockLocked();
}
void BackingPlayer::stop() {
    const juce::ScopedLock owner(lock);
    if (files.empty()) return;
    wantsPlay = false; playing.store(false);
    ++clockGeneration; renderObservation.valid = false; publishClockLocked();
}

void BackingPlayer::prepare(double sr, int bs, int latency) {
    const juce::ScopedLock owner(lock);
    outputReady = std::isfinite(sr) && sr >= 8000 && sr <= 384000 && bs > 0 && bs <= 16384;
    renderObservation.valid = false; ++renderObservation.routeGeneration;
    renderObservation.sampleRate = outputReady ? sr : 0;
    renderObservation.outputLatencyFrames = outputReady && latency >= 0 ? latency : -1;
    if (outputReady) { outputRate = sr; outputBlock = bs; }
    if (!files.empty()) {
        if (outputReady) scheduleLocked(cachedPosition.load(), false);
        else { playing.store(false); publishClockLocked(); }
    }
    else publishClockLocked();
}
void BackingPlayer::invalidateOutputTiming() {
    const juce::ScopedLock owner(lock);
    outputReady = false; playing.store(false);
    renderObservation.valid = false; renderObservation.outputLatencyFrames = -1;
    ++renderObservation.routeGeneration;
    publishClockLocked();
}

void BackingPlayer::run() {
    std::uint64_t seen = 0;
    while (!threadShouldExit()) {
        std::vector<juce::File> paths;
        Gains values;
        double position, rate, sr;
        int block;
        bool resetLeveler, fullLast = false;
        std::uint64_t song = 0;
        {
            const juce::ScopedLock owner(lock);
            if (requested.load() != seen) {
                seen = requested.load(); paths = files; values = gains;
                position = requestedPosition; rate = requestedRate; sr = outputRate;
                block = outputBlock; song = songSerial;
                resetLeveler = newSong || committedSong != song;
                fullLast = fullMixLast;
            }
        }
        if (paths.empty()) { wait(5); continue; }
        std::unique_ptr<BackingSession> next;
        bool ok = false;
        try {
            next = std::make_unique<BackingSession>(paths, sr, block, position, rate, values, fullLast);
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
            while (!threadShouldExit() && requested.load() == seen) {
                const auto status = next->prime();
                if (status == BackingSession::Result::audio) { ok = true; break; }
                if (status == BackingSession::Result::failed || juce::Time::getMillisecondCounterHiRes() >= deadline) break;
                wait(1);
            }
        } catch (...) { ok = false; }
        std::unique_ptr<BackingSession> retired;
        {
            const juce::ScopedLock owner(lock);
            if (!threadShouldExit() && requested.load() == seen) {
                if (ok && active && !resetLeveler && active->sampleRate() == sr) next->retainLeveler(*active);
                retired = std::move(active);
                if (ok) {
                    active = std::move(next); committed = seen; committedSong = song;
                    cachedDuration.store(active->duration()); cachedPosition.store(active->position());
                } else { cachedDuration.store(0); cachedPosition.store(0); }
                succeeded = ok; failed = !ok; playing.store(ok && wantsPlay && outputReady);
                renderObservation.valid = false; publishClockLocked(); completed.store(seen);
            }
        }
        // Both superseded and retired sessions are destroyed on THIS worker.
    }
    std::unique_ptr<BackingSession> retired;
    { const juce::ScopedLock owner(lock); retired = std::move(active); }
}

int BackingPlayer::renderBlockLocked(int samples, double callbackAtMs) {
    const auto started = callbackAtMs > 0 ? callbackAtMs : juce::Time::getMillisecondCounterHiRes();
    const int frames = std::clamp(samples, 0, active->blockSize());
    const double before = active->position();
    const auto status = active->render(frames, gains);
    analysis.append(active->buffer().getReadPointer(0), active->buffer().getReadPointer(1), static_cast<unsigned>(frames));
    buffering = status == BackingSession::Result::waiting;
    if (status == BackingSession::Result::audio) {
        cachedPosition.store(active->position());
        renderObservation.valid = true;
        renderObservation.startedAtMs = started;
        renderObservation.firstFramePosition = before;
        renderObservation.sourcePositionAfterRender = active->sourcePosition();
        renderObservation.sampleRate = active->sampleRate(); renderObservation.frames = frames;
        renderObservation.stretchInputLatencyFrames = active->inputLatency();
        renderObservation.stretchOutputLatencyFrames = active->outputLatency();
        if (active->position() >= active->duration())
            drainUntilMs = started + std::max(0, renderObservation.outputLatencyFrames) * 1000.0 / active->sampleRate()
                + (active->duration() - before) * 1000.0 / requestedRate;
    } else if (status == BackingSession::Result::ended && started < drainUntilMs) {
        // Keep the last output anchor until its final frame reaches the device.
        // The decoder/stretcher is finished; the hardware queue is not yet.
    } else if (status == BackingSession::Result::ended || status == BackingSession::Result::failed) {
        playing.store(false); wantsPlay = false; ended = status == BackingSession::Result::ended;
        failed = status == BackingSession::Result::failed;
        if (failed) renderObservation.valid = false;
    } // Waiting retains the last rendered anchor while queued output drains.
    publishClockLocked();
    return frames;
}
}
