#include "../../src/audio/SourceChain.h"
#include "../../src/audio/engine/InputChannelMeters.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <memory>

static void near(float actual, float expected)
{
    if (std::abs(actual - expected) > 0.0001f)
    {
        std::cerr << "expected " << expected << ", got " << actual << '\n';
        std::abort();
    }
}

int main()
{
    constexpr int frames = 480; // 10 ms at 48 kHz
    std::atomic<bool> running{true};
    std::atomic<double> sampleRate{48000};
    // Exercise broadcast, pair mix, higher input channel and zero-output capture.
    for (int outputChannels : {0, 1, 2})
    for (int selected : {-1, 0, 6})
    {
        auto source = std::make_unique<SourceChain>(0, running, sampleRate);
        source->prepare(48000, frames);
        source->setMonitorMute(false);
        source->setTonePolishEnabled(false);
        source->setNoiseGate(false, -60, 100, -60);
        source->setInputChannel(selected);
        juce::AudioBuffer<float> input(8, frames), output(outputChannels, frames);
        for (int ch = 0; ch < 8; ++ch)
            for (int i = 0; i < frames; ++i)
                input.setSample(ch, i, ch == 6 ? 0.2f : 0.1f);
        const float raw = selected == 6 ? 0.2f : 0.1f;
        auto process = [&] {
            source->processBlock(input.getArrayOfReadPointers(), 8, output, outputChannels, frames);
        };
        auto checkDry = [&](float expected) {
            for (float value : source->getInputFrame(frames)) near(value, expected);
            for (float value : source->getRawAudioFrame(frames)) near(value, expected);
        };
        auto checkMonitor = [&](float expected) {
            for (int ch = 0; ch < outputChannels; ++ch)
                for (int i = 0; i < frames; ++i) near(output.getSample(ch, i), expected);
        };

        source->setInputGain(3);
        process(); checkDry(raw * 3); checkMonitor(raw * 3); // legacy unchanged
        assert(source->setInputCalibrationGain(2));
        process(); // finish calibration's 10 ms ramp
        const auto ramp = source->getInputFrame(frames);
        near(ramp.front(), raw * (1.0f + 1.0f / frames));
        near(ramp.back(), raw * 2);
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            near(output.getSample(ch, 0), ramp.front() * 3);
            near(output.getSample(ch, frames - 1), raw * 2 * 3);
        }
        process(); checkDry(raw * 2); checkMonitor(raw * 2 * 3);
        if (outputChannels) near(source->getInputLevel(), raw * 2);
        source->setInputGain(5);
        process(); checkDry(raw * 2); checkMonitor(raw * 2 * 5);
        // Preset reload/reset changes tone drive only, even with an empty chain.
        source->getSignalChain().clear(); source->setInputGain(1);
        process(); checkDry(raw * 2); checkMonitor(raw * 2);
        near(source->getInputCalibrationGain(), 2);
        source->setMonitorKill(true);
        process(); checkDry(raw * 2); checkMonitor(0); // external hardware scoring
        // Hardware samples and raw channel observations never inherit calibration.
        slopsmith::InputChannelMeters meter;
        meter.prepare(8, 48000); meter.arm();
        meter.process(input.getArrayOfReadPointers(), 8, frames);
        near(meter.read()[6].peak, 0.2f);
        near(input.getSample(6, 0), 0.2f);
        source->releaseResources(); source->prepare(48000, frames);
        process(); checkDry(raw * 2); // device reopen preserves selected calibration
        source->releaseResources();
    }
    // The explicit API is source 0 only; independent sources retain legacy mode.
    auto source = std::make_unique<SourceChain>(1, running, sampleRate);
    source->prepare(48000, frames); source->setInputGain(4);
    juce::AudioBuffer<float> mono(1, frames), output(2, frames);
    for (int i = 0; i < frames; ++i) mono.setSample(0, i, 0.1f);
    source->processBlock(mono.getArrayOfReadPointers(), 1, output, 2, frames);
    for (float value : source->getInputFrame(frames)) near(value, 0.4f);
    source->releaseResources();
    std::cout << "Source capture calibration, tone isolation and legacy paths passed\n";
}
