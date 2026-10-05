#pragma once
#include <napi.h>
#include "../Whammy.h"

inline bool readWhammy(const Napi::Object& note, Whammy& out, double sustain)
{
    auto raw = note.Get("whammy");
    if (raw.IsUndefined()) return true;
    if (!raw.IsObject() || raw.IsNull() || raw.IsArray()) return false;
    auto w = raw.As<Napi::Object>();
    if (w.GetPropertyNames().Length() != 3 || !w.Get("version").IsNumber()
        || w.Get("version").As<Napi::Number>().DoubleValue() != 1
        || !w.Get("policy").IsString() || w.Get("policy").As<Napi::String>().Utf8Value() != "optional"
        || !w.Get("segments").IsArray()) return false;
    auto segments = w.Get("segments").As<Napi::Array>();
    if (segments.Length() < 1 || segments.Length() > 20000) return false;
    auto num = [](const Napi::Object& o, const char* key, double& target) {
        auto v = o.Get(key); if (!v.IsNumber()) return false;
        target = v.As<Napi::Number>().DoubleValue(); return std::isfinite(target);
    };
    for (uint32_t i=0; i<segments.Length(); ++i) {
        auto rawSegment = segments.Get(i);
        if (!rawSegment.IsObject() || rawSegment.IsNull() || rawSegment.IsArray()) return false;
        auto s = rawSegment.As<Napi::Object>(); Whammy::Segment segment;
        if (s.GetPropertyNames().Length() != (s.Has("vibrato") ? 6u : 5u)
            || !num(s,"start",segment.start) || !num(s,"end",segment.end)
            || !s.Get("source_id").IsString() || !s.Get("group").IsString() || !s.Get("curve").IsArray()) return false;
        segment.sourceId = s.Get("source_id").As<Napi::String>().Utf8Value();
        segment.group = s.Get("group").As<Napi::String>().Utf8Value();
        if (s.Has("vibrato")) {
            if (!s.Get("vibrato").IsString()) return false;
            segment.vibrato = s.Get("vibrato").As<Napi::String>().Utf8Value();
        }
        auto curve = s.Get("curve").As<Napi::Array>();
        if (curve.Length() > 2048) return false;
        for (uint32_t j=0; j<curve.Length(); ++j) {
            auto rawPoint = curve.Get(j);
            if (!rawPoint.IsObject() || rawPoint.IsNull() || rawPoint.IsArray()) return false;
            auto p = rawPoint.As<Napi::Object>(); Whammy::Point point;
            if (p.GetPropertyNames().Length() != 2 || !num(p,"t",point.t) || !num(p,"v",point.v)) return false;
            segment.curve.push_back(point);
        }
        out.segments.push_back(std::move(segment));
    }
    return out.valid(sustain);
}
