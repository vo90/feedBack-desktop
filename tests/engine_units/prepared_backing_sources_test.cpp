#include "../../src/audio/engine/PreparedBackingSources.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>

using Sources = slopsmith::PreparedBackingSources;

static void writeFixture(const juce::File& file, const std::vector<float>& values, double sr = 48000) {
    juce::WavAudioFormat format;
    auto stream = file.createOutputStream(); assert(stream);
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(format.createWriterFor(stream.release(), sr, 2, 24, {}, 0));
    assert(writer);
    juce::AudioBuffer<float> buffer(2, static_cast<int>(values.size()));
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
        buffer.setSample(0, i, values[i]); buffer.setSample(1, i, -values[i]);
    }
    assert(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()));
}

static void awaitPrepared(Sources& sources) {
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
    while (sources.state() == Sources::State::preparing && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::Thread::sleep(1);
    assert(sources.state() != Sources::State::preparing);
}

static void writeCompressedFixture(const juce::File& file, const std::vector<float>& values) {
    juce::OggVorbisAudioFormat format;
    auto stream = file.createOutputStream(); assert(stream);
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(format.createWriterFor(stream.release(), 48000, 2, 16, {}, 5));
    assert(writer);
    juce::AudioBuffer<float> buffer(2, static_cast<int>(values.size()));
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
        buffer.setSample(0, i, values[i]); buffer.setSample(1, i, -values[i]);
    }
    assert(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()));
}

static std::vector<float> consume(Sources& sources, const std::vector<float>& gains,
                                  std::vector<float>* rightResult = nullptr) {
    awaitPrepared(sources); assert(sources.state() == Sources::State::ready);
    std::vector<float> result;
    std::array<float, 257> left, right;
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        auto read = sources.read(left.data(), right.data(), left.size(), gains.data(), gains.data());
        assert(read.status != Sources::ReadStatus::failed);
        if (read.status == Sources::ReadStatus::end) return result;
        if (read.status == Sources::ReadStatus::waiting) {
            for (unsigned i = 0; i < left.size(); ++i) assert(left[i] == 0 && right[i] == 0);
            juce::Thread::sleep(1); continue;
        }
        for (unsigned i = 0; i < read.frames; ++i) {
            if (rightResult) rightResult->push_back(right[i]);
            else assert(std::abs(left[i] + right[i]) < 1e-6f);
            result.push_back(left[i]);
        }
        for (unsigned i = read.frames; i < left.size(); ++i) assert(left[i] == 0 && right[i] == 0);
    }
    assert(false && "decoder/consumer failed to finish within deadline"); return {};
}

int main() {
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("feedback-native-backing-test", "", false);
    assert(directory.createDirectory());
    constexpr int frames = 19713;
    std::vector<float> full(frames, 0);
    std::vector<std::vector<float>> stems;
    std::vector<juce::File> files;
    for (int stem = 0; stem < 6; ++stem) {
        stems.emplace_back(frames, 0.0f);
        // Exactly representable low-level markers at unrelated frame positions;
        // alignment, left/right routing, and channel gains are independently tested.
        for (int i = stem * 17; i < frames; i += 503 + stem * 2) {
            stems.back()[i] = float(stem + 1) / 128;
            full[i] += stems.back()[i];
        }
        files.push_back(directory.getChildFile(juce::String(stem) + ".wav"));
        writeFixture(files.back(), stems.back());
    }
    const auto fullFile = directory.getChildFile("full.wav"); writeFixture(fullFile, full);
    {
        Sources single({fullFile}, 48000, 0, 4096);
        Sources multiple(files, 48000, 0, 4096);
        const auto a = consume(single, {1});
        const auto b = consume(multiple, {1, 1, 1, 1, 1, 1});
        assert(a.size() == frames && b.size() == frames);
        for (int i = 0; i < frames; ++i) {
            assert(std::abs(a[i] - full[i]) < 1e-6f);
            assert(std::abs(a[i] - b[i]) < 1e-6f);
        }
        assert(single.consumedFrames() == frames && multiple.consumedFrames() == frames);
    }
    {
        Sources solo(files, 48000, 0, 4096);
        const auto a = consume(solo, {0, 0, 1, 0, 0, 0});
        for (int i = 0; i < frames; ++i) assert(std::abs(a[i] - stems[2][i]) < 1e-6f);
    }
    {
        // A new seek generation starts every source at the same output position.
        Sources sought(files, 48000, .25, 4096);
        const auto a = consume(sought, {1, 1, 1, 1, 1, 1});
        assert(a.size() == frames - 12000);
        assert(sought.startPosition() == .25);
        for (unsigned i = 0; i < a.size(); ++i) assert(std::abs(a[i] - full[i + 12000]) < 1e-6f);
    }
    {
        Sources resampled({fullFile}, 44100, 0, 4096);
        const auto a = consume(resampled, {1});
        assert(a.size() == static_cast<unsigned>(std::ceil(frames * 44100.0 / 48000)));
        for (auto value : a) assert(std::isfinite(value));
        Sources resampledStems(files, 44100, 0, 4096);
        const auto b = consume(resampledStems, {1, 1, 1, 1, 1, 1});
        assert(a.size() == b.size());
        for (unsigned i = 0; i < a.size(); ++i) assert(std::abs(a[i] - b[i]) < 1e-6f);
    }
    {
        const auto oggFile = directory.getChildFile("full.ogg"); writeCompressedFixture(oggFile, full);
        Sources compressed({oggFile}, 48000, 0, 4096);
        Sources compressedStems(std::vector<juce::File>(6, oggFile), 48000, 0, 4096);
        // Lossy encoding need not preserve exact L == -R. Compare both decoded
        // channels between paths, instead of asserting the source-file identity.
        std::vector<float> rightA, rightB;
        const auto a = consume(compressed, {1}, &rightA);
        const auto b = consume(compressedStems, std::vector<float>(6, 1.0f / 6), &rightB);
        assert(a.size() == frames && a.size() == b.size());
        for (unsigned i = 0; i < a.size(); ++i) {
            assert(std::abs(a[i] - b[i]) < 1e-6f);
            assert(std::abs(rightA[i] - rightB[i]) < 1e-6f);
        }
    }
    {
        // Different source rates still share one output-frame frontier.
        const auto halfRate = directory.getChildFile("half-rate.wav");
        writeFixture(halfRate, std::vector<float>(12000, .125f), 24000);
        Sources mixedRates({fullFile, halfRate}, 48000, 0, 4096);
        const auto a = consume(mixedRates, {0, 1});
        assert(a.size() == 24000);
        // Resampling filters legitimately shape the start/end boundary. The
        // entire waveform must match this same source played alone, while its
        // steady section retains the expected DC value.
        Sources halfAlone({halfRate}, 48000, 0, 4096);
        const auto b = consume(halfAlone, {1});
        assert(a.size() == b.size());
        for (unsigned i = 0; i < a.size(); ++i) assert(std::abs(a[i] - b[i]) < 1e-6f);
        for (unsigned i = 100; i + 64 < a.size(); ++i) assert(std::abs(a[i] - .125f) < 1e-5f);
    }
    {
        Sources maximum(std::vector<juce::File>(32, fullFile), 48000, 0, 4096);
        const auto a = consume(maximum, std::vector<float>(32, 1.0f / 32));
        assert(a.size() == frames);
        for (int i = 0; i < frames; ++i) assert(std::abs(a[i] - full[i]) < 1e-6f);
    }
    {
        const auto shortFile = directory.getChildFile("short.wav");
        writeFixture(shortFile, std::vector<float>(1000, .125f));
        Sources differentLengths({fullFile, shortFile}, 48000, 0, 4096);
        const auto a = consume(differentLengths, {0, 1});
        assert(a.size() == frames);
        for (int i = 0; i < frames; ++i) assert(std::abs(a[i] - (i < 1000 ? .125f : 0)) < 1e-6f);
    }
    {
        Sources missing({fullFile, directory.getChildFile("missing.wav")}, 48000);
        awaitPrepared(missing); assert(missing.state() == Sources::State::failed);
    }
    {
        Sources pastEnd(files, 48000, 999);
        assert(consume(pastEnd, {1, 1, 1, 1, 1, 1}).empty());
    }
    {
        Sources validation({fullFile}, 48000, 0, 4096);
        awaitPrepared(validation);
        float left[8]{}, right[8]{}, gain[]{1}, bad[]{std::numeric_limits<float>::quiet_NaN()};
        assert(validation.read(left, right, 0, gain, gain).status == Sources::ReadStatus::audio);
        assert(validation.read(left, right, 8, bad, gain).status == Sources::ReadStatus::failed);
        assert(validation.consumedFrames() == 0);
    }
    for (int i = 0; i < 20; ++i) {
        Sources cancelled(files, 48000, 0, 4096); // cancel during prepare/prefill
    }
    // This directory was uniquely created above and owns only these fixtures.
    assert(directory.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory));
    assert(directory.deleteRecursively());
    std::cout << "real worker decode: 1/6-source sample parity, gains, seek, resample, padding, errors and cancellation passed\n";
}
