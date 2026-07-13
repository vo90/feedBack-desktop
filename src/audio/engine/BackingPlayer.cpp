// BackingPlayer implementation — moved verbatim from AudioEngine.cpp (TLC
// plan phase 3 / §2.4); member names lose their backing prefixes, logic is
// unchanged. See BackingPlayer.h for the boundary rationale.

#include "BackingPlayer.h"

#include <cmath>
#include <iostream>

namespace slopsmith {

bool BackingPlayer::load(const juce::File& file)
{
    const juce::ScopedLock sl(lock);
    stopNoLock();
    transport.reset();
    readerSource.reset();

    const bool exists = file.existsAsFile();
    std::cerr << "[AudioEngine] loadBackingTrack path="
              << file.getFullPathName().toStdString()
              << " exists=" << exists
              << " size=" << (exists ? (long long) file.getSize() : -1)
              << std::endl;

    auto* reader = formatManager.createReaderFor(file);
    if (!reader)
    {
        std::cerr << "[AudioEngine] loadBackingTrack: no reader for ext='"
                  << file.getFileExtension().toStdString()
                  << "' (registered formats=" << formatManager.getNumKnownFormats()
                  << ")" << std::endl;
        // Transport/source already reset above; clear cached state so the renderer
        // doesn't keep displaying the previous track's position/duration.
        cachedPosition.store(0.0);
        cachedDuration.store(0.0);
        return false;
    }

    const double readerSampleRate = reader->sampleRate;
    const juce::int64 readerLengthInSamples = reader->lengthInSamples;
    const double sr = state.currentSampleRate.load(std::memory_order_relaxed);
    // Backing audio plays through the output device in both modes, so size
    // against outputBlockSize. In duplex mode outputBlockSize == inputBlockSize;
    // in split mode the output device's clock drives the backing pull.
    const int    bs = state.outputBlockSize.load(std::memory_order_relaxed);

    readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    transport = std::make_unique<juce::AudioTransportSource>();
    // Read-ahead on readThread so the RT audio thread normally never touches
    // the disk or the format codec. Previously this passed (…, 0, nullptr, …):
    // with no read-ahead buffer the transport decoded the file synchronously
    // inside getNextAudioBlock ON the audio callback, so any disk seek /
    // decode spike (worst for compressed formats) blew the block budget →
    // underruns heard as glitches or brief mutes while a song plays.
    // 32768 source frames ≈ 0.68 s @ 48k of look-ahead absorbs those spikes.
    //
    // Known residual (accepted): juce::BufferingAudioSource is not fully
    // RT-safe — readBufferSection() holds callbackLock across the decode of one
    // refill chunk, and the callback's getNextAudioBlock() takes the same lock,
    // so the RT thread can still block behind an in-flight chunk decode. The
    // window is bounded (JUCE caps chunks at 2048 source frames) and only hit
    // when a refill is mid-decode, vs. the old guaranteed full decode on every
    // block; a truly lock-free ring would mean replacing the JUCE transport
    // stack and isn't worth it here.
    // The 4th arg makes AudioTransportSource SRC the file to device rate.
    // Stretch always sees device-rate audio so that its presetDefault parameters match.
    constexpr int kReadAheadSamples = 32768;
    transport->setSource(readerSource.get(), kReadAheadSamples,
                         &readThread, readerSampleRate);

    // Loading a backing track before the audio device has started leaves
    // sr/bs at zero. presetDefault(2, 0.0f) would seed the stretcher with
    // undefined internal timing, and prepareToPlay(0, 0) is similarly
    // ill-defined. Defer the stretcher + buffer setup; the relevant
    // audio*AboutToStart() re-runs the same block (via prepare()) once a real
    // sample rate / block size are known.
    if (sr > 0.0 && bs > 0)
    {
        // prepareToPlay's first arg is an upper bound on subsequent
        // getNextAudioBlock requests, per the juce::AudioSource contract.
        // The RT callback can pull ceil(bs * kMaxSpeed) frames in a single
        // block when the speed is above 1×, so prepare for that worst case —
        // preparing with just `bs` would risk JUCE internal buffer
        // overruns/asserts on the first faster-than-1× block.
        const int maxInputFrames = (int) std::ceil(bs * kMaxSpeed) + 64;
        transport->prepareToPlay(maxInputFrames, sr);

        stretch.presetDefault(2, (float) sr);
        stretch.reset();
        stretchLatencySamples.store(stretch.outputLatency(), std::memory_order_relaxed);

        inputBuffer.setSize(2, maxInputFrames, false, false, true);
        outputBuffer.setSize(2, bs, false, false, true);
    }

    cachedDuration.store(transport->getLengthInSeconds());
    cachedPosition.store(0.0);
    heardPositionSec.store(0.0, std::memory_order_relaxed);

    // Reset the loudness leveler for the new song: clearing the cached sample
    // rate forces renderBlockLocked() to re-prepare() it on the next block,
    // dropping the previous track's AGC gain + limiter state. Otherwise the
    // ~300 ms gain follower would carry over and briefly mis-level the start
    // of a much louder/quieter next song. Safe here — load holds the lock,
    // the same lock the render path runs under.
    levelerSr = 0.0;
    std::cerr << "[AudioEngine] loadBackingTrack OK sr=" << readerSampleRate
              << " len=" << readerLengthInSamples
              << std::endl;
    return true;
}

void BackingPlayer::setPosition(double seconds)
{
    const juce::ScopedLock sl(lock);
    if (transport)
    {
        transport->setPosition(seconds);
        stretch.reset();
        // Read back the actual position; the transport may clamp (e.g. negative or past EOF).
        const double pos = transport->getCurrentPosition();
        cachedPosition.store(pos);
        heardPositionSec.store(pos, std::memory_order_relaxed);
    }
}

void BackingPlayer::start()
{
    const juce::ScopedLock sl(lock);
    if (transport)
    {
        transport->start();
        playing.store(true);
        heardPositionSec.store(transport->getCurrentPosition(),
                               std::memory_order_relaxed);
    }
}

void BackingPlayer::stopNoLock()
{
    if (transport)
    {
        transport->stop();
        stretch.reset();
        playing.store(false);
    }
}

void BackingPlayer::stop()
{
    const juce::ScopedLock sl(lock);
    stopNoLock();
}

void BackingPlayer::setSpeed(double newSpeed)
{
    if (!std::isfinite(newSpeed) || newSpeed <= 0.0)
    {
        return;
    }

    const double clamped = juce::jlimit(0.01, kMaxSpeed, newSpeed);
    // Dead-zone against the last *requested* rate to coalesce rapid slider
    // ticks — but never skip a change that crosses the 1× bypass boundary, or a
    // request just shy of 1× (e.g. 0.9995 -> 1.0, diff < 0.001) would leave the
    // stretcher path engaged when the caller actually asked for transparent
    // full speed.
    const double prev = pendingSpeed.load(std::memory_order_relaxed);
    const bool prevBypass = std::abs(prev    - 1.0) < kSpeedBypassEpsilon;
    const bool newBypass  = std::abs(clamped - 1.0) < kSpeedBypassEpsilon;
    if (std::abs(clamped - prev) < 0.001 && prevBypass == newBypass)
    {
        return;
    }

    // Lock-free hand-off to the audio thread. Publish the requested rate, then
    // raise the pending flag with release so the RT thread is guaranteed to see
    // the new rate once it observes the flag. renderBlockLocked() adopts the
    // rate and resets the stretcher together, on the audio thread, so:
    //   * a control-thread caller (e.g. a speed slider at 30-60 Hz) never takes
    //     the lock and so never starves the RT tryLock into dropping a block;
    //   * the new rate is never processed with stale stretch state — the reset
    //     and the rate adoption happen in the same RT block (see PR #237).
    // Multiple updates before the RT consumes them coalesce (latest wins), which
    // naturally throttles stretcher resets during a drag.
    pendingSpeed.store(clamped, std::memory_order_relaxed);
    speedChangePending.store(true, std::memory_order_release);
}

void BackingPlayer::prepare(double sr, int bs)
{
    const juce::ScopedLock sl(lock);
    if (transport && sr > 0.0 && bs > 0)
    {
        // See load() for why prepareToPlay uses maxInputFrames rather than bs:
        // the RT callback can pull ceil(bs * kMaxSpeed) frames in a single
        // block at faster-than-1× speeds.
        const int maxInputFrames = (int) std::ceil(bs * kMaxSpeed) + 64;
        transport->prepareToPlay(maxInputFrames, sr);
        stretch.presetDefault(2, (float) sr);
        stretch.reset();
        stretchLatencySamples.store(stretch.outputLatency(), std::memory_order_relaxed);
        inputBuffer.setSize(2, maxInputFrames, false, false, true);
        outputBuffer.setSize(2, bs, false, false, true);
    }
}

int BackingPlayer::renderBlockLocked(int numSamples)
{
    // Adopt any speed change requested since the last block (set lock-free by
    // setSpeed). Common (no-change) path is a plain acquire load — no locked
    // RMW, so the flag's cache line stays shared and isn't bounced to this
    // core every callback. Only the rare block that actually consumes a change
    // does the exchange (clearing the flag atomically so a concurrent setSpeed
    // can't lose an update). The acquire pairs with the release-store in
    // setSpeed so the new rate is visible here. Reset the stretcher and
    // re-anchor the heard position in the SAME block we adopt the rate, so a
    // block is never processed at the new rate with stale stretch state.
    // reset() only clears state (no allocation), so it's audio-thread safe.
    if (speedChangePending.load(std::memory_order_acquire))
    {
        speedChangePending.exchange(false, std::memory_order_acquire);
        speed.store(juce::jlimit(0.01, kMaxSpeed,
                                 pendingSpeed.load(std::memory_order_relaxed)),
                    std::memory_order_relaxed);
        stretch.reset();
        heardPositionSec.store(transport->getCurrentPosition(),
                               std::memory_order_relaxed);
    }

    const double rate = juce::jlimit(0.01, kMaxSpeed, speed.load(std::memory_order_relaxed));

    // Defensive clamp: the buffers are sized by prepare() from the device's
    // nominal block size, but a callback can deliver a larger numSamples on a
    // device-reconfig race. Drop the excess frames silently rather than
    // reading/writing past the allocated span; the next callback after
    // reconfig arrives at the new nominal size.
    const int outCap = outputBuffer.getNumSamples();
    const int inCap  = inputBuffer.getNumSamples();
    const int outSamples = juce::jmin(numSamples, outCap);
    const double sr = state.currentSampleRate.load(std::memory_order_relaxed);
    const bool bypassStretch = std::abs(rate - 1.0) < kSpeedBypassEpsilon;

    int sourceFramesPulled = 0;

    if (bypassStretch)
    {
        // 1× — direct transport read, no phase-vocoder path. (The transport
        // still sample-rate-converts the file to the device rate, so this is
        // "no time-stretch", not necessarily bit-perfect.)
        outputBuffer.clear(0, outSamples);
        juce::AudioSourceChannelInfo info(&outputBuffer, 0, outSamples);
        transport->getNextAudioBlock(info);
        sourceFramesPulled = outSamples;
    }
    else
    {
        // Slow/fast path — pull only the source frames needed for this output
        // block (output * rate), then stretch in-process to fill outSamples.
        const int inputFrames = juce::jmin((int) std::ceil(outSamples * rate), inCap);

        inputBuffer.clear(0, inputFrames);
        juce::AudioSourceChannelInfo info(&inputBuffer, 0, inputFrames);
        transport->getNextAudioBlock(info);
        sourceFramesPulled = inputFrames;

        outputBuffer.clear(0, outSamples);

        const float* const* inPtrs  = inputBuffer.getArrayOfReadPointers();
        float* const* outPtrs = outputBuffer.getArrayOfWritePointers();
        stretch.process(inPtrs, inputFrames, outPtrs, outSamples);
    }

    const double transportPos = transport->getCurrentPosition();
    if (sr > 0.0 && sourceFramesPulled > 0)
    {
        // Accumulate the heard (source) position, but clamp to the transport's
        // actual position. sourceFramesPulled is the requested block size; a
        // short read (e.g. at EOF, where the transport returns fewer real frames
        // and zero-pads) would otherwise advance the playhead past the true
        // source point and report progress beyond the track duration before
        // `playing` flips false. getCurrentPosition() stays clamped to the
        // real source position.
        double heard = heardPositionSec.load(std::memory_order_relaxed)
                       + static_cast<double>(sourceFramesPulled) / sr;
        heard = juce::jmin(heard, transportPos);
        heardPositionSec.store(heard, std::memory_order_relaxed);

        // Bypass reads straight from the transport — no phase-vocoder output
        // latency to compensate for. Only the stretch path adds latency.
        const double latencyInputSec = bypassStretch
            ? 0.0
            : (stretchLatencySamples.load(std::memory_order_relaxed) * rate) / sr;
        cachedPosition.store(juce::jmax(0.0, heard - latencyInputSec));
    }
    else
    {
        // currentSampleRate is transiently 0 during device teardown/reconfig.
        // We can't accumulate (no Hz to divide by), so anchor both the heard
        // accumulator and the published playhead to the real transport position
        // rather than leaving a stale value visible to the UI.
        heardPositionSec.store(transportPos, std::memory_order_relaxed);
        cachedPosition.store(juce::jmax(0.0, transportPos));
    }

    // Sync the flag if transport stopped at EOF.
    if (!transport->isPlaying())
        playing.store(false);

    // Normalize the backing track to a consistent target loudness (-12 LUFS)
    // BEFORE the mixer's backing-volume fader is applied (later in the RT
    // callback), so every song sits at the same level while the fader still
    // attenuates it. Standard BS.1770 K-weighting (full-mix music) + a brickwall
    // limiter to keep boosted peaks safe. RT-safe (no allocation).
    if (outSamples > 0 && sr > 0.0)
    {
        if (sr != levelerSr) { leveler.prepare(sr); levelerSr = sr; }
        leveler.process(outputBuffer, outSamples, -12.0f);
    }

    return outSamples;
}

} // namespace slopsmith
