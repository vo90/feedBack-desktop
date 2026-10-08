#include "../../src/audio/engine/BackingPlayer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

using namespace slopsmith;
static std::vector<float> render(BackingSession& session, const BackingSession::Gains& gains) {
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
    for (;;) {
        assert(juce::Time::getMillisecondCounterHiRes() < deadline);
        const auto r = session.prime();
        assert(r != BackingSession::Result::failed);
        if (r == BackingSession::Result::audio) break;
        juce::Thread::sleep(1);
    }
    std::vector<float> samples;
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        const double before = session.position();
        const auto result = session.render(257, gains);
        assert(result != BackingSession::Result::failed);
        if (result == BackingSession::Result::ended) return samples;
        if (result == BackingSession::Result::waiting) {
            assert(session.position() == before); juce::Thread::sleep(1); continue;
        }
        assert(session.position() >= before);
        auto* data = session.buffer().getReadPointer(0);
        samples.insert(samples.end(), data, data + 257);
    }
    assert(false); return {};
}
int main() {
    juce::TemporaryFile fixture(".wav");
    constexpr int frames = 48013;
    {
        auto stream = fixture.getFile().createOutputStream();
        juce::WavAudioFormat format;
        auto writer = std::unique_ptr<juce::AudioFormatWriter>(format.createWriterFor(stream.release(), 48000, 2, 24, {}, 0));
        assert(writer);
        juce::AudioBuffer<float> buffer(2, frames);
        for (int i = 0; i < frames; ++i) for (int ch = 0; ch < 2; ++ch)
            buffer.setSample(ch, i, .1f * std::sin(i * (2 * juce::MathConstants<double>::pi * 440 / 48000)));
        assert(writer->writeFromAudioSampleBuffer(buffer, 0, frames));
    }
    BackingSession::Gains one{}, six{}; one[0] = 1;
    std::fill_n(six.begin(), 6, 1.0f / 6);
    for (double speed : {.5, .73, 1.0, 1.5, 2.0, 4.0}) {
        BackingSession a({fixture.getFile()}, 48000, 257, 0, speed, one);
        BackingSession b(std::vector<juce::File>(6, fixture.getFile()), 48000, 257, 0, speed, six);
        const auto x = render(a, one), y = render(b, six);
        assert(x.size() == y.size());
        assert(a.renderedFrames() == static_cast<std::uint64_t>(std::ceil(frames / speed)));
        assert(a.position() == a.duration() && b.position() == b.duration());
        double tailEnergy = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            assert(std::isfinite(x[i]) && std::abs(x[i] - y[i]) < .0001f);
            if (i > x.size() - 1024) tailEnergy += x[i] * x[i];
        }
        assert(tailEnergy > .01); // no premature end when decoder reaches EOF
    }
    EngineState state;
    BackingPlayer player(state);
    const auto first = player.beginLoad({fixture.getFile()}, {1});
    const auto replacement = player.beginLoad(std::vector<juce::File>(6, fixture.getFile()), std::vector<float>(6, 1.0f/6));
    assert(!player.waitForRequest(first));
    assert(player.waitForRequest(replacement));
    player.start();
    const auto seek = player.beginSeek(.5);
    player.stop(); // preparation completion must not undo this pause
    assert(player.waitForRequest(seek) && !player.isPlaying());
    assert(player.getPosition() == .5);
    player.start();
    player.prepare(44100, 257, 800);
    assert(player.waitForRequest(player.currentRequestId()) && player.isPlaying());
    assert(player.getPosition() == .5);
    assert(!player.setSourceGains({1}));
    assert(player.setSourceGains({1, 0, 0, 0, 0, 0}));
    std::cout << "shared session: 1/6-source parity at six rates, fractional accounting, tails and cancellation passed\n";
}
