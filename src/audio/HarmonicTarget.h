#pragma once
#include <cmath>
#include <string>

// Fretted targets are distinct from natural harmonics above the open string.
struct HarmonicTarget
{
    std::string kind;
    double node = 0;
    int interval = 0;
    std::string policy;
    bool present() const { return !kind.empty(); }
    bool feedback() const { return kind == "feedback"; }
    bool mixed() const { return kind == "semi"; }
    bool valid() const
    {
        if (!present()) return interval == 0 && node == 0 && policy.empty();
        const bool known = kind == "pinch" || kind == "artificial" || kind == "tapped"
            || kind == "semi" || kind == "feedback";
        if (!known || !std::isfinite(node)
            || policy != (feedback() ? "attack_either" : mixed() ? "mixed" : "harmonic")) return false;
        const double nodes[] = {12,7,19,5,24,4,9,16,3.2,2.7,5.8,9.6,14.7,21.7,2.4,8.2,17};
        const int pitches[] = {12,19,19,24,24,28,28,28,31,34,34,34,34,34,36,36,36};
        for (int i = 0; i < 17; ++i) if (node == nodes[i]) return interval == pitches[i];
        return false;
    }
};
