#pragma once
#include "HarmonicTarget.h"
#include <algorithm>

// A delayed contact belongs to one ordinary picked attack. It is not another
// verifier queue entry and never retunes that attack's target.
struct HarmonicContact
{
    bool enabled = false;
    double start = 0, end = 0;
    HarmonicTarget target;
    std::string sourceId;
    bool present() const { return enabled; }
    bool valid(double sustain, int fret) const
    {
        if (!enabled) return start == 0 && end == 0 && !target.present() && sourceId.empty();
        return std::isfinite(sustain) && std::isfinite(start) && std::isfinite(end)
            && start > 0 && start < end && std::abs(end-sustain) <= .0000011
            && fret >= 0 && fret <= 48 && target.valid() && target.kind == "artificial"
            && !sourceId.empty() && sourceId.size() <= 512;
    }
    double attackEnd(double tolerance, double grace) const
    {
        return enabled ? std::min(tolerance+grace,start) : tolerance+grace;
    }
};
