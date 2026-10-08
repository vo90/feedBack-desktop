#include "../../src/audio/InputCaptureClock.h"
#include "../../src/audio/engine/BackingAnalysis.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <thread>
#include <iostream>
int main() {
    InputCaptureClock clock;
    assert(!clock.timeFor(0, 48000));
    clock.publish(4800, 1000);
    assert(clock.timeFor(4320, 48000).value() == 990);
    assert(!clock.timeFor(4801, 48000));
    assert(!clock.timeFor(0, 0));
    clock.publish(0, 0); assert(!clock.timeFor(0, 48000));
    slopsmith::BackingAnalysis tap;
    assert(!tap.read().valid);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        float block[64];
        for (int start = 1; start < 640000; start += 64) {
            for (int i = 0; i < 64; ++i) block[i] = float(start + i);
            tap.append(block, block, 64);
            clock.publish(start + 63, (start + 63) / 48.0);
        }
        done.store(true);
    });
    do {
        const auto snapshot = tap.read();
        if (snapshot.valid) for (unsigned i = 1; i < snapshot.samples.size(); ++i)
            assert(snapshot.samples[i] == 0 || snapshot.samples[i] == snapshot.samples[i - 1] + 1);
        if (const auto time = clock.timeFor(64000, 48000)) assert(std::abs(*time - 64000 / 48.0) < 1e-8);
    } while (!done.load());
    writer.join();
    std::cout << "bounded capture timestamps and coherent concurrent analysis snapshots passed\n";
}
