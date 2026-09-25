#pragma once
#include "HarmonicTargetBinding.h"
#include "../HarmonicContact.h"

inline bool readHarmonicContact(const Napi::Object& note, HarmonicContact& out, double sustain, int fret)
{
    auto raw=note.Get("harmonic_changes");
    if (raw.IsUndefined()) return true;
    for (const char* key : {"hm","hp","mt","fhm"})
        if (note.Has(key) && note.Get(key).ToBoolean().Value()) return false;
    for (const char* key : {"harmonic_target","hn","hps","harmonic_alias"})
        if (!note.Get(key).IsUndefined()) return false;
    if (!raw.IsObject() || raw.IsNull() || raw.IsArray()) return false;
    auto c=raw.As<Napi::Object>();
    if (c.GetPropertyNames().Length()!=2 || !c.Get("version").IsNumber()
        || c.Get("version").As<Napi::Number>().DoubleValue()!=1 || !c.Get("events").IsArray()) return false;
    auto events=c.Get("events").As<Napi::Array>();
    if (events.Length()!=1) return false;
    auto value=events.Get(uint32_t(0));
    if (!value.IsObject() || value.IsNull() || value.IsArray()) return false;
    auto e=value.As<Napi::Object>();
    if (e.GetPropertyNames().Length()!=4 || !e.Get("start").IsNumber() || !e.Get("end").IsNumber()
        || !e.Get("source_id").IsString()) return false;
    out.enabled=true;
    out.start=e.Get("start").As<Napi::Number>().DoubleValue();
    out.end=e.Get("end").As<Napi::Number>().DoubleValue();
    out.sourceId=e.Get("source_id").As<Napi::String>().Utf8Value();
    auto target=Napi::Object::New(note.Env());
    target.Set("f",note.Get("f")); target.Set("harmonic_target",e.Get("target"));
    return readHarmonicTarget(target,out.target,false) && out.valid(sustain,fret);
}
