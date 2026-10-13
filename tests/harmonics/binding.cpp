#include "../../src/audio/addon/HarmonicTargetBinding.h"

static Napi::Object initialize(Napi::Env env, Napi::Object exports) {
    exports.Set("read",Napi::Function::New(env,[](const Napi::CallbackInfo& info) -> Napi::Value {
        HarmonicTarget target;
        const bool ok=info.Length()>0 && info[0].IsObject()
            && readHarmonicTarget(info[0].As<Napi::Object>(),target,false);
        auto result=Napi::Object::New(info.Env());
        result.Set("ok",ok);
        if(ok && target.present()) {
            result.Set("node",target.node ? Napi::Number::New(info.Env(),*target.node).As<Napi::Value>() : info.Env().Null().As<Napi::Value>());
            result.Set("interval",target.interval);
            result.Set("basis",target.basis);
        }
        return result;
    }));
    return exports;
}
NODE_API_MODULE(harmonic_target_binding, initialize)
