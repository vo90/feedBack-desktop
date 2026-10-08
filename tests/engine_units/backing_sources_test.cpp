#include "../../src/audio/engine/BackingSourceQueue.h"
#include "../../src/audio/engine/BackingTiming.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <iostream>
#include <limits>
#include <thread>

using namespace slopsmith;

static void queueTests() {
    for (auto dimensions : {std::pair{0u, 8u}, {33u, 8u}, {1u, 0u}, {1u, 262145u}}) {
        bool rejected = false;
        try { BackingSourceQueue q(dimensions.first, dimensions.second); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    BackingSourceQueue q(2, 5);
    const float a[]{1, 2, 3, 4, 5}, b[]{10, 20, 30, 40, 50};
    const float* data[]{a, a, b, b};
    const float all[]{1, 1}, solo[]{0, 1};
    float l[6]{}, r[6]{};
    assert(q.write(data, 4));
    assert(!q.write(data, 2));
    assert(!q.mix(l, r, 5, all, all) && q.consumedFrames() == 0);
    assert(q.mix(l, r, 3, all, all));
    assert(l[0] == 11 && l[2] == 33 && r[2] == 33);
    assert(q.write(data, 4)); // wrap both producer and consumer
    assert(q.mix(l, r, 5, solo, solo));
    const float expected[]{40, 10, 20, 30, 40};
    for (int i = 0; i < 5; ++i) assert(l[i] == expected[i] && r[i] == expected[i]);
    assert(q.consumedFrames() == 8 && q.availableToRead() == 0);
    assert(q.write(data, 4));
    assert(q.mix(l, r, 4, all, solo));
    assert(l[0] == 10.75f && l[3] == 40); // sample-continuous gain ramp
    assert(!q.write(nullptr, 1));
    const float* bad[]{a, a, b, nullptr};
    assert(!q.write(bad, 1));

    BackingSourceQueue crossfade(3, 5);
    const float full[]{11, 22, 33, 44, 55};
    const float* withFull[]{a, a, b, b, full, full};
    const float fromFull[]{0, 0, 1}, toStems[]{1, 1, 0};
    assert(crossfade.write(withFull, 4));
    assert(crossfade.mix(l, r, 4, fromFull, toStems));
    for (int i = 0; i < 4; ++i) assert(l[i] == full[i] && r[i] == full[i]);

    // Real concurrent producer/consumer: every track encodes the same absolute
    // frame number. Torn publication or independent stem progress fails exactly.
    BackingSourceQueue concurrent(6, 127);
    constexpr unsigned total = 200000;
    std::thread producer([&] {
        std::array<std::array<float, 31>, 12> samples;
        std::array<const float*, 12> pointers;
        for (unsigned c = 0; c < 12; ++c) pointers[c] = samples[c].data();
        for (unsigned frame = 0; frame < total;) {
            const auto count = std::min(31u, total - frame);
            for (unsigned c = 0; c < 12; ++c)
                for (unsigned i = 0; i < count; ++i) samples[c][i] = float(frame + i);
            if (concurrent.write(pointers.data(), count)) frame += count;
            else std::this_thread::yield();
        }
    });
    const float gains[]{1, 1, 1, 1, 1, 1};
    std::array<float, 23> left, right;
    for (unsigned frame = 0; frame < total;) {
        const auto count = std::min(23u, total - frame);
        if (!concurrent.mix(left.data(), right.data(), count, gains, gains)) {
            std::this_thread::yield(); continue;
        }
        for (unsigned i = 0; i < count; ++i) {
            assert(left[i] == float(frame + i) * 6);
            assert(right[i] == left[i]);
        }
        frame += count;
    }
    producer.join();
    assert(concurrent.consumedFrames() == total);
}

static void timingTests() {
    for (double rate : {.5, .75, 1.0, 1.5, 2.0, 4.0}) {
        const auto near = [](std::optional<double> actual, double expected) {
            assert(actual && std::abs(*actual - expected) < 1e-10);
        };
        BackingPresentationAnchor a{10, 1000, 0, rate, 3, 9};
        // Same song note heard on two outputs. Detection has 2 ms capture age.
        // Slower output is represented entirely by the -500 ms calibration.
        near(backingSongTimeForDetection(a, 1002, 2, 0, 3, 9), 10);
        near(backingSongTimeForDetection(a, 1502, 2, -500, 3, 9), 10);
        // A detector adding 30 ms processing changes event age, NOT the judgment.
        near(backingSongTimeForDetection(a, 1532, 32, -500, 3, 9), 10);
        // Once 100 ms of driver delay is known, residual compensation is -400.
        // Keeping -500 would double-count that 100 ms and fail this comparison.
        a.outputLatencyMs = 100;
        near(backingSongTimeForDetection(a, 1502, 2, -400, 3, 9), 10);
        near(backingSongTimeAt(a, 1500, -400, 3, 9), 10);
        near(backingSongTimeAt(a, 1600, -400, 3, 9), 10 + .1 * rate);
        near(backingSongTimeAt(a, 1000, 100, 3, 9), 10); // positive residual
        assert(!backingSongTimeAt(a, 1500, 0, 4, 9)); // stale seek/song
        assert(!backingSongTimeAt(a, 1500, 0, 3, 10)); // stale device
        assert(!backingSongTimeForDetection(a, 1502, -2, 0, 3, 9));
    }
    BackingPresentationAnchor beginning{0, 1000, 500, 1, 1, 1};
    assert(*backingSongTimeAt(beginning, 1000, 0, 1, 1) == -.5);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(!backingSongTimeAt(beginning, nan, 0, 1, 1));
    assert(!backingSongTimeAt(beginning, 1000, nan, 1, 1));
    beginning.outputLatencyMs = -1;
    assert(!backingSongTimeAt(beginning, 1000, 0, 1, 1)); // unknown is not zero
}

int main() {
    queueTests(); timingTests();
    std::cout << "bounded multistem queue, concurrent alignment, and capture/output timing passed\n";
}
