#include "BackingSession.h"
#include "BackingFrameMath.h"
#include <cmath>

namespace slopsmith {
BackingSession::BackingSession(const std::vector<juce::File>& files, double sampleRate,
    int blockSize, double position, double speed, const Gains& gains, bool fullMixLast)
    : sr(sampleRate), rate(speed), block(blockSize), bypass(std::abs(speed - 1) < 1e-4), start(position), currentGains(gains) {
    if (!std::isfinite(sr) || sr < 8000 || sr > 384000 || block <= 0 || block > 16384
        || !std::isfinite(rate) || rate < .01 || rate > 4)
        throw std::invalid_argument("Unsupported backing session format");
    stretch.presetDefault(2, static_cast<float>(sr));
    const int maxInput = std::max(static_cast<int>(std::ceil(block * 4.0)) + 64, stretch.inputLatency());
    input.setSize(2, maxInput); output.setSize(2, block);
    unsigned capacity = BackingSourceQueue::defaultCapacity;
    while (capacity < static_cast<unsigned>(maxInput * 2)) capacity *= 2;
    sources = std::make_unique<PreparedBackingSources>(files, sr, position, capacity, fullMixLast);
    leveler.prepare(sr);
}

BackingSession::Result BackingSession::prime() {
    if (sources->state() == PreparedBackingSources::State::failed) return Result::failed;
    if (sources->state() != PreparedBackingSources::State::ready) return Result::waiting;
    if (primeState == 0) {
        start = std::clamp(start, 0.0, duration());
        totalOutput = static_cast<std::uint64_t>(ceilFrameBoundary(std::max(0.0, duration() - start) * sr / rate));
        if (!bypass) {
            const auto read = sources->read(input.getWritePointer(0), input.getWritePointer(1),
                stretch.inputLatency(), currentGains.data(), currentGains.data());
            if (read.status == PreparedBackingSources::ReadStatus::waiting) return Result::waiting;
            if (read.status == PreparedBackingSources::ReadStatus::failed) return Result::failed;
            // Look ahead on the worker. This removes the algorithm's input
            // preroll before the first audible block instead of delaying play.
            stretch.seek(input.getArrayOfReadPointers(), stretch.inputLatency(), rate);
        }
        primeState = 1;
    }
    while (!bypass && discarded < stretch.outputLatency()) {
        const int count = std::min(block, stretch.outputLatency() - discarded);
        const auto result = process(count, currentGains, true);
        if (result != Result::audio) return result;
        discarded += count;
    }
    primeState = 2;
    return Result::audio;
}

BackingSession::Result BackingSession::process(int frames, const Gains& gains, bool discard) {
    const double inputCount = bypass ? frames : frames * rate + fraction;
    const int pull = static_cast<int>(floorFrameBoundary(inputCount));
    auto& destination = bypass ? output : input;
    const auto read = sources->read(destination.getWritePointer(0), destination.getWritePointer(1),
        pull, currentGains.data(), gains.data());
    if (read.status == PreparedBackingSources::ReadStatus::waiting) { output.clear(); return Result::waiting; }
    if (read.status == PreparedBackingSources::ReadStatus::failed) { output.clear(); return Result::failed; }
    // End/short reads supply zero padding. Continue processing it until the
    // audible song duration is drained; decoding EOF is not playback EOF.
    fraction = bypass ? 0 : std::max(0.0, inputCount - pull);
    currentGains = gains;
    if (!bypass) stretch.process(input.getArrayOfReadPointers(), pull, output.getArrayOfWritePointers(), frames);
    if (!discard) {
        leveler.process(output, frames, -12.0f);
        rendered += frames;
    }
    return Result::audio;
}

BackingSession::Result BackingSession::render(int frames, const Gains& gains) {
    output.clear();
    if (primeState != 2 || frames < 0 || frames > block) return Result::failed;
    if (rendered >= totalOutput) return Result::ended;
    const int count = static_cast<int>(std::min<std::uint64_t>(frames, totalOutput - rendered));
    return process(count, gains, false);
}
}
