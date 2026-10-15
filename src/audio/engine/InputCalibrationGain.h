#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace slopsmith {

// Dedicated capture calibration, independent of tone/preset drive. A zero target
// means legacy mode until the first valid explicit setter. One atomic publishes
// activation + target together; ramp state belongs exclusively to the audio thread.
class InputCalibrationGain
{
public:
    bool setGain(double gain) noexcept
    {
        if (!std::isfinite(gain) || gain < 0.1 || gain > 5.0) return false;
        requested.store(static_cast<float>(gain), std::memory_order_release);
        return true;
    }

    float getGain() const noexcept
    {
        const float value = requested.load(std::memory_order_acquire);
        return value > 0.0f ? value : 1.0f;
    }

    // Called only while the device callback is stopped. Reopening a device keeps
    // the saved target; no transition is needed before audio has started.
    void prepare(double sampleRate) noexcept
    {
        rampSamples = std::isfinite(sampleRate) && sampleRate > 0.0
            ? static_cast<int>(std::clamp(sampleRate * 0.010, 1.0, 100000.0)) : 480;
        current = target = getGain();
        remaining = 0;
    }

    bool beginBlock() noexcept
    {
        const float value = requested.load(std::memory_order_acquire);
        if (value == 0.0f) return false;
        if (value != target)
        {
            target = value;
            remaining = rampSamples;
            step = (target - current) / static_cast<float>(remaining);
        }
        return true;
    }

    // Advance ONCE per sample, then use the same value for every output channel.
    float nextGain() noexcept
    {
        if (remaining > 0)
        {
            current += step;
            if (--remaining == 0) current = target;
        }
        return current;
    }

private:
    std::atomic<float> requested{0.0f};
    float current = 1.0f, target = 1.0f, step = 0.0f;
    int remaining = 0, rampSamples = 480;
};

} // namespace slopsmith
