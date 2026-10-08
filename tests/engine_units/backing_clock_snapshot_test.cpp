#include "../../src/audio/engine/BackingClockSnapshot.h"
#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <mutex>
#include <thread>
#include <iostream>

int main() {
    slopsmith::BackingClockSnapshot clock;
    assert(!clock.read().valid);
    clock.publish(10, 1000, .5, 7, true, false);
    const auto a = clock.read();
    assert(a.valid && a.position == 10 && a.sampledAtMs == 1000 && a.rate == .5);
    assert(a.sequence == 1 && a.generation == 7 && a.playing && !a.ended);
    clock.publish(9.999, 1010, 1, 8, false, true);
    const auto b = clock.read();
    assert(b.valid && b.sequence == 2 && b.generation == 8 && b.ended && !b.playing);
    assert(b.position == 9.999);
    assert(!b.render.valid && b.render.outputLatencyFrames == -1);

    // Match production: multiple potential publishers, serialized by an
    // existing owner lock, with a concurrent lock-free reader.
    slopsmith::BackingClockSnapshot concurrent;
    std::mutex owner;
    std::uint64_t serial = 0;
    std::atomic<int> finished{0};
    const auto publish = [&] {
        for (int i = 0; i < 50000; ++i) {
            const std::lock_guard<std::mutex> guard(owner);
            const auto n = ++serial;
            slopsmith::BackingRenderObservation render;
            render.valid = true; render.routeGeneration = n;
            render.startedAtMs = n * 4.0; render.sourcePositionAfterRender = n * 5.0;
            render.sampleRate = n * 6.0; render.frames = int(n);
            render.outputLatencyFrames = int(n) + 1;
            render.stretchInputLatencyFrames = int(n) + 2;
            render.stretchOutputLatencyFrames = int(n) + 3;
            concurrent.publish(double(n), double(n * 2), double(n * 3), n, n % 2, !(n % 2), render);
        }
        ++finished;
    };
    std::thread control(publish), audio(publish);
    std::uint64_t previous = 0, reads = 0;
    while (finished.load() != 2) {
        const auto s = concurrent.read();
        if (!s.valid) continue;
        assert(s.sequence >= previous && s.sequence == s.generation);
        assert(s.position == double(s.generation));
        assert(s.sampledAtMs == s.position * 2 && s.rate == s.position * 3);
        assert(s.playing == bool(s.generation % 2) && s.ended != s.playing);
        assert(s.render.valid && s.render.routeGeneration == s.generation);
        assert(s.render.startedAtMs == s.position * 4 && s.render.sourcePositionAfterRender == s.position * 5);
        assert(s.render.sampleRate == s.position * 6 && s.render.frames == s.generation);
        assert(s.render.outputLatencyFrames == s.generation + 1);
        assert(s.render.stretchInputLatencyFrames == s.generation + 2);
        assert(s.render.stretchOutputLatencyFrames == s.generation + 3);
        previous = s.sequence; ++reads;
    }
    control.join(); audio.join();
    assert(concurrent.read().sequence == 100000);
    assert(reads > 0);
    std::cout << "coherent snapshots across 100000 publications; " << reads << " accepted concurrent reads\n";
}
