// Phase 2 unit tests for RendererBus (docs/audio-engine-tlc.md §5):
// resampler continuity across pushes, equal-rate bit-exactness, the prime
// gate, underflow → silence + re-prime, fill clamp, and metrics arithmetic.
// The flush-on-disable test flips once the phase-8 flush-flag fix lands.

#include "../../src/audio/engine/RendererBus.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using slopsmith::RendererBus;
static constexpr int kReserveFrames = 960; // 20 ms at the 48 kHz test rate

static std::vector<float> rampChunk(int frames, float start, float step)
{
    std::vector<float> v((size_t) frames * 2);
    for (int i = 0; i < frames; ++i)
    {
        v[(size_t) i * 2]     = start + step * (float) i;
        v[(size_t) i * 2 + 1] = -(start + step * (float) i);
    }
    return v;
}

// Equal rates degenerate to step == 1.0 — frames must come out bit-exact
// (minus the one-frame interpolation carry at each chunk boundary).
static void testEqualRateBitExact()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto c1 = rampChunk(512, 0.0f, 1.0f);
    const auto c2 = rampChunk(1025, 512.0f, 1.0f);
    assert(bus.push(c1.data(), 512, 48000.0, 48000.0));
    assert(bus.push(c2.data(), 1025, 48000.0, 48000.0));

    std::vector<float> dl(512), dr(512);
    assert(bus.pull(dl.data(), dr.data(), 512) == 512);
    for (int i = 0; i < 512; ++i)
    {
        // First chunk's frame 0 is consumed as interpolation carry (pos
        // starts at 0 with prev=0 carry → exact frame i lands at output i).
        assert(dl[(size_t) i] == (float) i && dr[(size_t) i] == -(float) i);
    }
}

// Downsampling 2:1 across a chunk seam must be continuous: the interpolated
// ramp has no discontinuity where one push ends and the next begins.
static void testResampleContinuityAcrossPushes()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const double src = 96000.0, dev = 48000.0;
    // Two chunks big enough that the 2:1 output (~1792 frames) clears the
    // prime gate; the seam sits at output frame ~512.
    const auto c1 = rampChunk(1024, 0.0f, 1.0f);
    const auto c2 = rampChunk(2560, 1024.0f, 1.0f);
    bus.push(c1.data(), 1024, src, dev);
    bus.push(c2.data(), 2560, src, dev);

    std::vector<float> dl(768), dr(768);
    assert(bus.pull(dl.data(), dr.data(), 768) == 768);
    for (int i = 1; i < 768; ++i)
    {
        const float d = dl[(size_t) i] - dl[(size_t) i - 1];
        // A linear ramp resampled 2:1 must step by ~2 everywhere, including
        // across the seam at output frame ~128.
        assert(std::fabs(d - 2.0f) < 1e-3f && "discontinuity at chunk seam");
    }
}

// Prime gate: nothing comes out until a complete block and reserve are buffered.
static void testPrimeGate()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    std::vector<float> dl(64), dr(64);
    const auto tiny = rampChunk(kReserveFrames / 2, 1.0f, 0.0f);
    bus.push(tiny.data(), kReserveFrames / 2, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 64) == 0 && "must gate until primed");
    bus.push(tiny.data(), kReserveFrames / 2, 48000.0, 48000.0);
    // Cushion built (minus the 1-frame carry per push) — next pull flows.
    bus.push(tiny.data(), kReserveFrames / 2, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 64) == 64);
}

// Underflow: whole-block silence, buffered tail dropped, back to priming.
static void testUnderflowReprimes()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto chunk = rampChunk(1473, 1.0f, 0.0f);
    bus.push(chunk.data(), 1473, 48000.0, 48000.0);
    std::vector<float> dl(512), dr(512);
    assert(bus.pull(dl.data(), dr.data(), 512) == 512);
    assert(bus.pull(dl.data(), dr.data(), 512) == 512);
    // Ring now nearly empty → this pull underflows.
    assert(bus.pull(dl.data(), dr.data(), 512) == 0);
    assert(bus.metrics().underflowCount == 1);
    // And the gate re-armed: a sub-prime refill still gates.
    const auto tiny = rampChunk(64, 1.0f, 0.0f);
    bus.push(tiny.data(), 64, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 32) == 0 && "must re-prime after underflow");
}

// Backlogs are trimmed to a complete output block plus scheduling reserve.
static void testFillClampTrimsBacklog()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const int backlog = 4096 + 2048;
    const auto chunk = rampChunk(backlog + 1, 1.0f, 0.0f);
    bus.push(chunk.data(), backlog + 1, 48000.0, 48000.0);
    std::vector<float> dl(256), dr(256);
    assert(bus.pull(dl.data(), dr.data(), 256) == 256);
    const auto m = bus.metrics();
    assert(m.overflowCount == 1 && "fill clamp must count as overflow");
    assert(m.fillFrames <= kReserveFrames && "backlog must be trimmed to prime target");
}

// Disabled bus: push and pull are inert.
static void testDisabledIsInert()
{
    RendererBus bus;
    const auto chunk = rampChunk(128, 1.0f, 0.0f);
    assert(!bus.push(chunk.data(), 128, 48000.0, 48000.0));
    std::vector<float> dl(64), dr(64);
    assert(bus.pull(dl.data(), dr.data(), 64) == 0);
    assert(!bus.metrics().enabled);
}

// Gain is applied consumer-side and sanitized (0..8, non-finite → 0).
static void testGainApplied()
{
    RendererBus bus;
    bus.setEnabled(true, 2.0f);
    const auto chunk = rampChunk(kReserveFrames + 65, 1.0f, 0.0f);
    bus.push(chunk.data(), kReserveFrames + 65, 48000.0, 48000.0);
    std::vector<float> dl(64), dr(64);
    assert(bus.pull(dl.data(), dr.data(), 64) == 64);
    assert(dl[0] == 2.0f && dr[0] == -2.0f);
}

// Disable drops the buffered tail — via the consumer-honored flush flag
// (deep-read §4 fix), so a re-enable never replays stale audio.
static void testFlushOnDisable()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto chunk = rampChunk(kReserveFrames * 2, 5.0f, 0.0f);
    bus.push(chunk.data(), kReserveFrames * 2, 48000.0, 48000.0);
    bus.setEnabled(false, 1.0f);   // requests the flush; consumer performs it
    bus.setEnabled(true, 1.0f);
    std::vector<float> dl(64), dr(64);
    // First pull consumes the flush: the pre-disable tail is gone, so the bus
    // is empty and (re-)priming — nothing plays.
    assert(bus.pull(dl.data(), dr.data(), 64) == 0 && "stale tail must not replay");
    assert(bus.metrics().fillFrames == 0 && "flush must drop the buffered tail");
    // Fresh audio after the re-enable flows once primed.
    const auto fresh = rampChunk(kReserveFrames + 65, 7.0f, 0.0f);
    bus.push(fresh.data(), kReserveFrames + 65, 48000.0, 48000.0);
    assert(bus.pull(dl.data(), dr.data(), 64) == 64);
    // Frame 0 is the resampler's one-frame interpolation carry (by design);
    // everything after must be the fresh push, not the flushed 5.0 tail.
    assert(dl[1] == 7.0f && "post-re-enable audio must be the fresh push");
}

// A pending flush must drop the STALE tail only. If no output callback runs
// between the disable and a re-enable (stopped device, device swap), the
// flush is still pending when fresh audio arrives — flushing to the live
// writeIndex at that point would discard the re-enabled bus's first frames
// too, silencing it until it re-primed. The flush target is snapshotted at
// disable time instead.
static void testFlushSparesPostReEnableAudio()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto stale = rampChunk(kReserveFrames * 2, 5.0f, 0.0f);
    bus.push(stale.data(), kReserveFrames * 2, 48000.0, 48000.0);

    // Disable + re-enable with NO pull in between: the flush is still pending.
    bus.setEnabled(false, 1.0f);
    bus.setEnabled(true, 1.0f);

    // Fresh audio pushed while the flush is still pending must survive it.
    const auto fresh = rampChunk(kReserveFrames + 65, 7.0f, 0.0f);
    bus.push(fresh.data(), kReserveFrames + 65, 48000.0, 48000.0);

    std::vector<float> dl(64), dr(64);
    assert(bus.pull(dl.data(), dr.data(), 64) == 64
           && "fresh post-re-enable audio must not be flushed away with the stale tail");
    // Frame 0 is the resampler's one-frame interpolation carry (by design);
    // everything after must be the fresh push, never the flushed 5.0 tail.
    assert(dl[1] == 7.0f && "flush must drop only the pre-disable tail");
}

// Rate validation (PR #107 review): non-finite rates cross the JS/IPC
// boundary; NaN passes a plain `<= 0` check, and a subnormal source rate can
// underflow step to 0 — both must be rejected before the resample loop.
// A bad sourceRate falls back to deviceRate (documented behaviour).
static void testRejectsUnusableRates()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    const auto chunk = rampChunk(128, 1.0f, 0.0f);
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    assert(!bus.push(chunk.data(), 128, 48000.0, nan));
    assert(!bus.push(chunk.data(), 128, 48000.0, inf));
    assert(!bus.push(chunk.data(), 128, 48000.0, -48000.0));
    assert(!bus.push(chunk.data(), 128, 48000.0, 0.0));
    // step underflow: denormal source over huge device rate → step == 0.
    assert(!bus.push(chunk.data(), 128, 5e-324, 1e308));
    assert(bus.metrics().pushedFrames == 0 && "rejected pushes must stage nothing");
    // NaN/Inf/negative SOURCE rate falls back to deviceRate (step == 1).
    assert(bus.push(chunk.data(), 128, nan, 48000.0));
    assert(bus.push(chunk.data(), 128, inf, 48000.0));
    assert(bus.push(chunk.data(), 128, -1.0, 48000.0));
    assert(bus.metrics().pushedFrames > 0);
}

static void testLargeBlocksKeepPartialPrime()
{
    for (int block : {64, 256, 480, 512, 2048, 4096})
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0})
        {
            RendererBus bus;
            bus.setEnabled(true, 1.0f);
            const int chunkSize = (int) std::ceil(rate / 200.0);
            const int reserve = std::max(block, (int) std::ceil(rate * 0.020));
            const auto chunk = rampChunk(chunkSize, 0.25f, 0.0f);
            std::vector<float> l(block), r(block);
            bool started = false;
            for (int i = 0; i < 100; ++i)
            {
                bus.push(chunk.data(), chunkSize, rate, rate);
                const int before = bus.metrics().fillFrames;
                const int pulled = bus.pull(l.data(), r.data(), block);
                if (pulled)
                {
                    assert(pulled == block && l.back() == 0.25f && r.back() == -0.25f);
                    assert(bus.metrics().fillFrames >= reserve);
                    started = true;
                    break;
                }
                assert(bus.metrics().fillFrames == before && "partial prime must accumulate, not be discarded");
                assert(bus.metrics().underflowCount == 0);
            }
            assert(started && "every supported block/rate must reach the prime gate");
        }
}

static void testStallRecoveryLatencyBound()
{
    for (int block : {64, 480, 2048, 4096})
        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            RendererBus bus;
            bus.setEnabled(true, 1.0f);
            const int reserve = std::max(block, (int) std::ceil(rate * 0.020));
            const int backlog = block + reserve * 8;
            const auto chunk = rampChunk(backlog + 1, 0.125f, 0.0f);
            bus.push(chunk.data(), backlog + 1, rate, rate);
            std::vector<float> l(block), r(block);
            assert(bus.pull(l.data(), r.data(), block) == block);
            assert(bus.metrics().fillFrames == reserve && "stall recovery must retain only the scheduling reserve");
            assert(bus.metrics().overflowCount == 1);
            // After recovery, an ordinary block at a time runs indefinitely
            // without clamping again or gradually accumulating delay.
            const auto next = rampChunk(block, 0.125f, 0.0f);
            for (int i = 0; i < 100; ++i)
            {
                bus.push(next.data(), block, rate, rate);
                assert(bus.pull(l.data(), r.data(), block) == block);
                assert(bus.metrics().fillFrames == reserve);
            }
            assert(bus.metrics().underflowCount == 0);
            assert(bus.metrics().overflowCount == 1);
        }
}

static void testBackToBackLargeCallbacks()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    constexpr int block = 2048;
    const auto backlog = rampChunk(block * 5 + 1, 0.25f, 0.0f);
    bus.push(backlog.data(), block * 5 + 1, 48000.0, 48000.0);
    std::vector<float> l(block), r(block);
    // Clamp a stale backlog, then survive a second callback arriving before
    // the renderer has delivered another chunk. This happens with large ASIO
    // buffers even though the long-term producer and consumer rates match.
    assert(bus.pull(l.data(), r.data(), block) == block);
    assert(bus.pull(l.data(), r.data(), block) == block);
    assert(bus.metrics().underflowCount == 0);
}

static void testSmallBlocksSurviveDeliveryJitter()
{
    RendererBus bus;
    bus.setEnabled(true, 1.0f);
    constexpr int block = 480;
    const auto initial = rampChunk(block * 3 + 1, 0.25f, 0.0f);
    const auto burst = rampChunk(block * 3, 0.25f, 0.0f);
    std::vector<float> l(block), r(block);
    bus.push(initial.data(), block * 3 + 1, 48000.0, 48000.0);
    for (int round = 0; round < 100; ++round)
    {
        // Three 10 ms output callbacks can run before the IPC producer gets
        // its next turn. Keep every frame and tolerate this phase variation.
        for (int i = 0; i < 3; ++i)
            assert(bus.pull(l.data(), r.data(), block) == block);
        bus.push(burst.data(), block * 3, 48000.0, 48000.0);
    }
    assert(bus.metrics().underflowCount == 0);
    assert(bus.metrics().overflowCount == 0);
}

int main()
{
    testEqualRateBitExact();
    testRejectsUnusableRates();
    testResampleContinuityAcrossPushes();
    testPrimeGate();
    testUnderflowReprimes();
    testFillClampTrimsBacklog();
    testDisabledIsInert();
    testGainApplied();
    testFlushOnDisable();
    testFlushSparesPostReEnableAudio();
    testLargeBlocksKeepPartialPrime();
    testStallRecoveryLatencyBound();
    testBackToBackLargeCallbacks();
    testSmallBlocksSurviveDeliveryJitter();
    std::puts("renderer_bus: all cases passed");
    return 0;
}
