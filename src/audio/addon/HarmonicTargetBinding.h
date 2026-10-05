#pragma once
#include <napi.h>
#include "../HarmonicTarget.h"

inline bool readHarmonicTarget(const Napi::Object& note, HarmonicTarget& out, bool natural)
{
    if (!note.Has("harmonic_target") || note.Get("harmonic_target").IsUndefined()) return true;
    auto value = note.Get("harmonic_target");
    if (natural || !value.IsObject() || value.IsArray() || value.IsNull()) return false;
    if (!note.Get("f").IsNumber()) return false;
    const double fret = note.Get("f").As<Napi::Number>().DoubleValue();
    if (!std::isfinite(fret) || fret < 0 || fret > 48 || fret != std::floor(fret)) return false;
    auto obj = value.As<Napi::Object>();
    if (obj.GetPropertyNames().Length() != 4 || !obj.Get("kind").IsString()
        || !obj.Get("node").IsNumber() || !obj.Get("interval").IsNumber()
        || !obj.Get("policy").IsString()) return false;
    double pitch = obj.Get("interval").As<Napi::Number>().DoubleValue();
    if (!std::isfinite(pitch) || pitch < 1 || pitch > 48 || pitch != std::floor(pitch)) return false;
    out.kind = obj.Get("kind").As<Napi::String>().Utf8Value();
    out.node = obj.Get("node").As<Napi::Number>().DoubleValue();
    out.interval = (int) pitch;
    out.policy = obj.Get("policy").As<Napi::String>().Utf8Value();
    return out.present() && out.valid();
}
