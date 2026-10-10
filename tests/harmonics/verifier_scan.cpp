#include "NoteVerifier.h"
#include "InputRingReader.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <tuple>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Run the real verifier synchronously, without devices, worker scheduling or
// wall-clock interpolation. The reference takes the existing full-scan fallback;
// both paths still execute the actual scorer, finalization and onset claiming.
struct NoteVerifierTestAccess {
    static void step(NoteVerifier& verifier, double time, bool fullScan = false, double rate = 1) {
        verifier.setPlayhead(time, false, rate);
        if (fullScan) verifier.canStopAtFutureNote = false;
        verifier.run();
    }
    static bool bounded(const NoteVerifier& verifier) { return verifier.canStopAtFutureNote; }
    static size_t ranges(const NoteVerifier& verifier) { return verifier.scanRanges.size(); }
    static void onsets(NoteVerifier& verifier) {
        verifier.onsetResetPending.store(false);
        verifier.onsetLog = {{.95, false}, {1.02, false}, {1.97, false}, {2.04, false}};
    }
    static void equivalent(const NoteVerifier& actual, const NoteVerifier& reference) {
        require(actual.state.size() == reference.state.size(), "state count differs");
        for (size_t i = 0; i < actual.state.size(); ++i) {
            const auto& a = actual.state[i]; const auto& b = reference.state[i];
            require(std::tie(a.finalized, a.detected, a.detectedSongTime, a.centsError, a.snr,
                a.everPresent, a.presentFrames, a.scoredFrames, a.bestSnr, a.bestCents, a.bestTargetFret)
                == std::tie(b.finalized, b.detected, b.detectedSongTime, b.centsError, b.snr,
                b.everPresent, b.presentFrames, b.scoredFrames, b.bestSnr, b.bestCents, b.bestTargetFret),
                "per-note scoring state differs");
        }
        require(actual.pending.size() == reference.pending.size(), "verdict count differs");
        for (size_t i = 0; i < actual.pending.size(); ++i) {
            const auto& a = actual.pending[i]; const auto& b = reference.pending[i];
            require(std::tie(a.id, a.detected, a.detectedSongTime, a.centsError, a.snr, a.targetFret)
                == std::tie(b.id, b.detected, b.detectedSongTime, b.centsError, b.snr, b.targetFret),
                "verdict value/order differs");
        }
        require(actual.onsetLog.size() == reference.onsetLog.size(), "onset count differs");
        for (size_t i = 0; i < actual.onsetLog.size(); ++i)
            require(actual.onsetLog[i].songTime == reference.onsetLog[i].songTime
                && actual.onsetLog[i].claimed == reference.onsetLog[i].claimed, "onset claiming differs");
    }
};

class Ring final : public InputRingReader {
public:
    bool signal = true;
    mutable std::function<void()> frameHook;
    std::vector<float> getInputFrame(int count) const override {
        if (frameHook) { auto hook = std::move(frameHook); frameHook = {}; hook(); }
        std::vector<float> result((size_t) count);
        if (signal) for (int i = 0; i < count; ++i)
            result[(size_t) i] = (float) (.3 * std::sin(2 * 3.141592653589793 * 82.406889 * i / 48000));
        return result;
    }
    uint64_t getInputSince(uint64_t from, std::vector<float>& out) const override { out.clear(); return from; }
    double getCurrentSampleRate() const override { return 48000; }
};

static NoteVerifier::ChartUpdate chart(size_t count, double spacing = .5) {
    NoteVerifier::ChartUpdate result;
    result.tuningOffsets.assign(6, 0);
    for (size_t i = 0; i < count; ++i) {
        NoteVerifier::ChartNote note;
        note.id = std::to_string(i); note.t = 1 + i * spacing;
        note.string = 0; note.fret = 0; note.sus = (i % 3) * .4;
        result.notes.push_back(note);
    }
    return result;
}

static void equivalenceTests() {
    Ring ring;
    NoteVerifier fast(ring), reference(ring);
    auto update = chart(20);
    // Equal-time chord notes must keep their original order when they compete
    // for onsets. A long early sustain keeps the prefix open past shorter notes.
    update.notes[0].sus = 4;
    update.notes[1].t = 1; update.notes[1].sus = 0;
    update.notes[2].t = 1; update.notes[2].sus = 0;
    update.notes[3].harmonicContact.enabled = true;
    update.notes[3].harmonicContact.start = .05;
    update.notes[3].sus = 2;
    update.notes[3].harmonicContact.end = 2;
    update.notes[3].harmonicContact.target = {"artificial", 7, 19, "harmonic"};
    update.notes[3].harmonicContact.sourceId = "delayed-contact";
    Whammy::Segment bar;
    bar.start = 0; bar.end = update.notes[4].sus;
    bar.sourceId = "bar"; bar.group = "bar"; bar.curve = {{0, 0}, {bar.end, -2}};
    update.notes[4].whammy.segments.push_back(bar);
    update.notes[5].harmonicTarget = {"feedback", 12, 12, "attack_either"};
    update.presenceRatio = .3f;
    auto install = [&](const NoteVerifier::ChartUpdate& next) {
        fast.setChart(next); reference.setChart(next);
        NoteVerifierTestAccess::onsets(fast); NoteVerifierTestAccess::onsets(reference);
    };
    size_t steps = 0;
    auto step = [&](double time, bool drain = false, double rate = 1.) {
        NoteVerifierTestAccess::step(fast, time, false, rate);
        NoteVerifierTestAccess::step(reference, time, true, rate);
        NoteVerifierTestAccess::equivalent(fast, reference);
        if (drain) { fast.drainVerdicts(); reference.drainVerdicts(); }
        ++steps;
    };
    install(update);
    require(NoteVerifierTestAccess::bounded(fast), "chronological chart should use bounds");
    // Exact inclusive start, strict ordinary deadline, inclusive contact
    // deadline, late seeks, pause/repeated samples, small backwards jitter,
    // jump threshold and reopening while verdicts are still pending.
    for (double t : {0., .899999, .9, 1., 1., 1.1, 1.100001, 1.5, 2., 2.1, 2.100001,
                     2.4, 2.55, 2.550001, 3.1, 8., 7.9, 7.65, 7.399, .9, 1., 2.5, 30.}) step(t);

    // Match the real producer: chronological singles followed by chronological
    // chord constituents. Equal-time notes in the two runs retain original
    // onset-claim order. Check both sides of every opening/deadline boundary.
    auto mixed = update;
    for (const auto& single : update.notes) for (int string = 0; string < 2; ++string) {
        auto note = single; note.id += ":chord:" + std::to_string(string);
        mixed.notes.push_back(note);
    }
    install(mixed);
    require(NoteVerifierTestAccess::bounded(fast) && NoteVerifierTestAccess::ranges(fast) == 2,
        "mixed singles/chords must use two bounded runs");
    std::vector<double> boundaries;
    for (const auto& note : mixed.notes) {
        const double grace = note.harmonicTarget.feedback() || note.whammy.present()
            ? 0 : std::clamp(note.sus, 0., 1.);
        for (double boundary : {note.t - mixed.timingTolerance,
                 note.t + note.harmonicContact.attackEnd(mixed.timingTolerance, grace)}) {
            boundaries.push_back(std::nextafter(boundary, -std::numeric_limits<double>::infinity()));
            boundaries.push_back(boundary);
            boundaries.push_back(std::nextafter(boundary, std::numeric_limits<double>::infinity()));
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (double time : boundaries) step(time);
    step(.9); step(1.); step(30., true);
    // Same-size chart generation replacement and changed timing tolerance.
    update.timingTolerance = .35;
    update.notes[0].id = "replacement";
    install(update);
    for (double t : {.64, .65, 1., 1.35, 2., 2.55, 50., 0., 1., 50.}) step(t, true, .5);
    fast.clearChart(); reference.clearChart(); step(100.);
    install(chart(0)); step(0.);
    install(chart(1)); step(20.); step(0.); step(1.); step(20., true);

    // Ordinary misses finalize strictly after the deadline, once per pass.
    ring.signal = false;
    install(chart(1)); step(.9); step(1.1);
    require(fast.drainVerdicts().empty() && reference.drainVerdicts().empty(), "deadline must remain inclusive");
    step(std::nextafter(1.1, std::numeric_limits<double>::infinity()));
    const auto misses = fast.drainVerdicts(); reference.drainVerdicts();
    require(misses.size() == 1 && !misses[0].detected, "crossing deadline must produce one miss");
    step(2.);
    require(fast.drainVerdicts().empty() && reference.drainVerdicts().empty(), "finalized note must not repeat");
    ring.signal = true;

    // Replace the same-size chart after candidate collection but before the
    // second lock. The generation guard must discard the old results and the
    // replacement must scan from its own beginning despite an advanced prefix.
    install(chart(100)); step(40., true);
    auto replacement = chart(100);
    replacement.timingTolerance = .2;
    replacement.notes[0].id = "mid-pass-replacement";
    ring.frameHook = [&] { fast.setChart(replacement); };
    NoteVerifierTestAccess::step(fast, 40.5);
    ring.frameHook = [&] { reference.setChart(replacement); };
    NoteVerifierTestAccess::step(reference, 40.5, true);
    require(!ring.frameHook, "mid-pass chart replacement was not exercised");
    NoteVerifierTestAccess::equivalent(fast, reference);
    step(1.); step(2.5); step(60., true);

    // Highly unordered, negative and non-finite windows retain exact fallback.
    for (int kind = 0; kind < 5; ++kind) {
        auto odd = chart(kind == 0 ? 70 : 4);
        if (kind == 0) std::reverse(odd.notes.begin(), odd.notes.end());
        if (kind == 1) odd.timingTolerance = -.5;
        if (kind == 2) odd.notes[1].t = std::numeric_limits<double>::quiet_NaN();
        if (kind == 3) odd.notes[1].t = std::numeric_limits<double>::infinity();
        if (kind == 4) odd.timingTolerance = std::numeric_limits<double>::infinity();
        install(odd);
        require(!NoteVerifierTestAccess::bounded(fast), "unusual chart must retain full scan");
        // Avoid intentionally invalid notes reaching the DSP scorer.
        ring.signal = false;
        for (double t : {-2., 0., 10., 5., 0., 10.}) step(t, true);
    }
    ring.signal = true;
    // Deterministic random timelines include overshoots, loop restarts, small
    // reversals and replacements with different sustains and tolerances.
    std::mt19937 random(91827);
    for (int generation = 0; generation < 8; ++generation) {
        auto next = chart(80, .13);
        next.timingTolerance = .02 + (random() % 25) / 100.;
        for (auto& note : next.notes) note.sus = (random() % 220) / 100.;
        if (generation % 2) std::shuffle(next.notes.begin(), next.notes.end(), random);
        install(next);
        double time = 0;
        for (int i = 0; i < 120; ++i) {
            time += (random() % 35) / 100.;
            if (i % 19 == 0) time = (random() % 1200) / 100.;
            if (i % 13 == 0) time -= .1;
            step(time, i % 7 == 0, i % 2 ? 1.5 : .5);
        }
    }
    std::cout << "Verifier full-scan equivalence passed: " << steps << " deterministic ticks\n";
}

static void benchmark() {
    Ring ring; ring.signal = false;
    for (int runs : {1, 2}) for (size_t count : {3053, 10000, 100000}) {
        NoteVerifier fast(ring), reference(ring);
        auto update = chart(count, .5);
        for (auto& note : update.notes) note.sus = 0;
        if (runs == 2) {
            auto ordered = update.notes;
            update.notes.clear();
            for (size_t parity = 0; parity < 2; ++parity)
                for (size_t i = parity; i < ordered.size(); i += 2) update.notes.push_back(ordered[i]);
        }
        fast.setChart(update); reference.setChart(update);
        // Finalize a prefix, then sit between note windows. This isolates the
        // repeated scan/lock work without changing audio DSP or using a device.
        const double time = 1 + (count / 2) * .5 + .3;
        NoteVerifierTestAccess::step(fast, time);
        NoteVerifierTestAccess::step(reference, time, true);
        fast.drainVerdicts(); reference.drainVerdicts();
        NoteVerifierTestAccess::equivalent(fast, reference);
        auto measure = [&](NoteVerifier& verifier, bool fullScan) {
            std::vector<double> samples;
            for (int trial = 0; trial < 7; ++trial) {
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < 1000; ++i) NoteVerifierTestAccess::step(verifier, time, fullScan);
                samples.push_back(std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - start).count() / 1000);
            }
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        };
        const auto oldUs = measure(reference, true), boundedUs = measure(fast, false);
        std::cout << count << " notes, " << runs << " runs: full=" << oldUs << " us/tick, bounded=" << boundedUs
            << " us/tick, ratio=" << oldUs / boundedUs << "\n";
    }
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--benchmark") benchmark();
        else equivalenceTests();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
