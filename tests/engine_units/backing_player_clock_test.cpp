#include "../../src/audio/engine/BackingPlayer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

int main() {
    // Exercise the real transport and callback without opening an audio device.
    juce::ScopedJuceInitialiser_GUI initialiser;
    juce::TemporaryFile fixture(".wav");
    {
        auto stream = fixture.getFile().createOutputStream();
        assert(stream);
        juce::WavAudioFormat format;
        auto writer = std::unique_ptr<juce::AudioFormatWriter>(format.createWriterFor(
            stream.release(), 48000, 2, 16, {}, 0));
        assert(writer);
        juce::AudioBuffer<float> samples(2, 48000); samples.clear();
        assert(writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples()));
    }
    slopsmith::EngineState state;
    slopsmith::BackingPlayer player(state);
    player.prepare(48000, 256);
    player.invalidateOutputTiming();
    assert(!player.getClockSnapshot().valid); // no fabricated track before load
    assert(player.load(fixture.getFile()));
    auto loaded = player.getClockSnapshot();
    assert(loaded.valid && loaded.position == 0 && !loaded.playing && !loaded.ended);
    player.start();
    auto started = player.getClockSnapshot();
    assert(started.playing && started.generation > loaded.generation);
    const auto block = [&] {
        const juce::ScopedLock owner(player.getLock());
        assert(player.readyLocked());
        assert(player.renderBlockLocked(256) == 256);
        return player.getClockSnapshot();
    };
    auto advancing = block();
    assert(advancing.valid && advancing.sequence > started.sequence && advancing.position > 0);
    assert(advancing.sampledAtMs >= started.sampledAtMs);
    assert(advancing.render.valid && advancing.render.frames == 256);
    assert(advancing.render.sampleRate == 48000 && advancing.render.outputLatencyFrames == -1);
    assert(advancing.render.stretchInputLatencyFrames == 0 && advancing.render.stretchOutputLatencyFrames == 0);
    assert(advancing.render.sourcePositionAfterRender == advancing.position);
    assert(advancing.render.startedAtMs <= advancing.sampledAtMs);
    state.currentSampleRate.store(44100);
    player.prepare(44100, 256, 512);
    const auto reconfigured = player.getClockSnapshot();
    assert(reconfigured.valid && reconfigured.generation > advancing.generation);
    assert(reconfigured.position == advancing.position && reconfigured.playing);
    assert(!reconfigured.render.valid && reconfigured.render.outputLatencyFrames == 512);
    assert(reconfigured.render.routeGeneration > advancing.render.routeGeneration);
    assert(block().position > reconfigured.position);
    player.setPosition(.5);
    auto sought = player.getClockSnapshot();
    assert(sought.position == .5 && sought.generation > advancing.generation && !sought.ended);
    player.setSpeed(.5);
    auto slow = block();
    assert(slow.rate == .5 && slow.generation > sought.generation);
    assert(slow.render.valid && slow.render.stretchInputLatencyFrames > 0 && slow.render.stretchOutputLatencyFrames > 0);
    assert(slow.render.outputLatencyFrames == 512); // no extra buffer period added
    player.stop();
    auto stopped = player.getClockSnapshot();
    assert(!stopped.playing && !stopped.ended && stopped.generation > slow.generation);
    assert(!stopped.render.valid);
    player.invalidateOutputTiming();
    const auto outputStopped = player.getClockSnapshot();
    assert(!outputStopped.render.valid && outputStopped.render.outputLatencyFrames == -1);
    assert(outputStopped.render.routeGeneration > stopped.render.routeGeneration);
    const auto frozenSequence = outputStopped.sequence;
    juce::Thread::sleep(5);
    assert(player.getClockSnapshot().sequence == frozenSequence);
    player.setPosition(.99); player.start();
    for (int i = 0; i < 200 && player.isPlaying(); ++i) block();
    const auto ended = player.getClockSnapshot();
    assert(!ended.playing && ended.ended);
    assert(ended.position <= player.getDuration());
    assert(!player.load(fixture.getFile().getSiblingFile("missing-clock-fixture.wav")));
    const auto missing = player.getClockSnapshot();
    assert(missing.valid && missing.position == 0 && !missing.playing && !missing.ended);
    std::cout << "real backing transport snapshot: load, play, seek, rate, pause, EOF and failed load passed\n";
}
