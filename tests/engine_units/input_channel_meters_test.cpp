#include "../../src/audio/engine/InputChannelMeters.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <iostream>
int main() {
    slopsmith::InputChannelMeters meter;
    meter.prepare(64, 48000);
    float signal[256]; std::fill_n(signal, 256, 0.25f);
    const float* channels[64] = {};
    channels[6] = signal; channels[63] = signal;
    meter.process(channels, 64, 256);
    assert(meter.getSequence() == 0); // idle: no metering work
    meter.arm(); meter.process(channels, 64, 256);
    auto levels = meter.read();
    assert(levels.size() == 64 && levels[0].peak == 0);
    assert(levels[6].peak == 0.25f && levels[6].rms == 0.25f && levels[63].peak == 0.25f);
    assert(signal[0] == 0.25f); // observational, never modifies audio
    for (int i=0;i<400;++i) meter.process(channels,64,256);
    const auto expired=meter.getSequence(); meter.process(channels,64,256);
    assert(meter.getSequence()==expired); // lease expires after UI closes
    meter.prepare(1,48000); meter.arm(); signal[0]=std::numeric_limits<float>::quiet_NaN();
    const float* mono[]={signal}; meter.process(mono,1,256);
    assert(std::isfinite(meter.read()[0].rms));
    meter.prepare(0,48000); meter.arm(); meter.process(nullptr,0,256);
    assert(meter.read().empty());
    std::cout << "Input channel metering passed\n";
}
