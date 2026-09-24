#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Authored signed pitch offsets, in note-relative song seconds. This model
// never synthesizes a vibrato pitch from a qualitative slight/wide marking.
struct Whammy
{
    struct Point { double t = 0, v = 0; };
    struct Segment {
        double start = 0, end = 0;
        std::string sourceId, group, vibrato;
        std::vector<Point> curve;
    };
    std::vector<Segment> segments;
    bool present() const { return !segments.empty(); }
    std::string limitation(bool bend, bool slide, bool impreciseHarmonic) const {
        if (!present()) return {};
        for (const auto& s : segments) if (!s.vibrato.empty()) return "bar_vibrato_without_exact_pitch";
        if (bend || slide) return "combined_bar_pitch_gestures";
        if (impreciseHarmonic) return "bar_harmonic_without_exact_pitch";
        return {};
    }
    bool valid(double sustain) const {
        if (!std::isfinite(sustain) || sustain < 0 || segments.size() > 20000) return false;
        double previous = 0;
        for (const auto& s : segments) {
            if (!std::isfinite(s.start) || !std::isfinite(s.end)
                || s.start < -0.0000011 || s.start < previous - 0.0000011
                || s.end <= s.start || s.end > sustain + 0.0000011
                || s.sourceId.empty() || s.sourceId.size() > 256 || s.group.empty() || s.group.size() > 256
                || (!s.vibrato.empty() && s.vibrato != "slight" && s.vibrato != "wide")
                || s.curve.size() > 2048 || (s.curve.empty() && s.vibrato.empty())) return false;
            previous = s.end;
            double last = -INFINITY;
            for (const auto& p : s.curve) {
                if (!std::isfinite(p.t) || !std::isfinite(p.v) || p.v < -16 || p.v > 8
                    || p.t < s.start - 0.0000011 || p.t > s.end + 0.0000011 || p.t <= last) return false;
                last = p.t;
            }
        }
        return true;
    }
    const Segment* at(double elapsed) const {
        if (!std::isfinite(elapsed)) return nullptr;
        elapsed = std::max(0., elapsed);
        for (auto it = segments.rbegin(); it != segments.rend(); ++it)
            if (elapsed >= it->start - 0.0000011 && elapsed <= it->end + 0.0000011) return &*it;
        return nullptr;
    }
    double pitch(double elapsed) const {
        const auto* s = at(elapsed);
        if (!s || s->curve.empty()) return 0;
        const auto& p = s->curve;
        if (elapsed <= p.front().t) return p.front().v;
        for (size_t i = 1; i < p.size(); ++i)
            if (elapsed <= p[i].t) return p[i-1].v + (p[i].v-p[i-1].v)*(elapsed-p[i-1].t)/(p[i].t-p[i-1].t);
        return p.back().v;
    }
};
