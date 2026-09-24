#include "ChordScorer.h"
#include "NoteVerifier.h"
#include "InputRingReader.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

static Whammy bar(double shift, const std::string& group="beat@0") {
    Whammy w; w.segments.push_back({0,2,"fixture:beat",group,"",{{0,shift},{2,shift}}}); return w;
}
static std::vector<float> signal(const std::vector<double>& midi) {
    std::vector<float> samples(4096);
    for(double m:midi) {
        const double hz=440*std::pow(2.,(m-69)/12);
        for(size_t i=0;i<samples.size();++i) samples[i]+=(float)(.2*std::sin(2*3.141592653589793*hz*i/48000)
            +.04*std::sin(4*3.141592653589793*hz*i/48000));
    }
    return samples;
}
class FixtureRing final : public InputRingReader {
public:
    explicit FixtureRing(double midi) : samples(signal({midi})) {}
    std::vector<float> getInputFrame(int) const override { return samples; }
    uint64_t getInputSince(uint64_t index,std::vector<float>& out) const override { out.clear();return index; }
    double getCurrentSampleRate() const override { return 48000; }
private:
    std::vector<float> samples;
};
int main() {
    ChordScorer scorer; int checks=0;
    for(bool bass:{false,true}) for(int capo:{0,2}) for(int tuning:{0,-2}) for(int fret:{0,7,12,24})
    for(double shift:{-16.,-8.,-2.,-.5,0.,.5,2.,8.}) for(bool harmonic:{false,true}) {
        ChordScorer::Request req;req.arrangement=bass?"bass":"guitar";req.stringCount=bass?4:6;
        req.capo=capo;req.tuningOffsets.assign(req.stringCount,tuning);req.pitchCheckCents=35;req.minHitRatio=1;
        ChordScorer::Note n;n.string=0;n.fret=fret;n.whammy=bar(shift);n.sustain=2;n.elapsed=.05;
        if(harmonic)n.harmonicTarget={"tapped",12,12,"harmonic"};
        req.notes={n};
        double original=(bass?28:40)+tuning+capo+fret+(harmonic?12:0);
        for(double played:{0.,shift,shift-12.,shift+12.,-999.}) {
            auto samples=signal(played==-999?std::vector<double>{}:std::vector<double>{original+played});
            const auto result=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
            const bool excluded=original+std::min(0.,shift)<22;
            const bool expected=!excluded && (played==0 || played==shift);
            if(result.isHit!=expected || result.totalStrings!=(excluded?0:1) || result.results[0].fret!=fret
                || (excluded && result.results[0].exclusionReason!="bar_pitch_below_verified_range")) {
                std::cerr<<"bar mismatch bass "<<bass<<" fret "<<fret<<" capo "<<capo<<" tuning "<<tuning<<" shift "<<shift<<" harmonic "<<harmonic<<" played "<<played<<" got "<<result.isHit<<"\n";return 1;
            } ++checks;
        }
    }
    // A two-string chord must not combine incompatible bar states into two
    // hits. Test both legitimate alternatives plus a wrong intermediate.
    ChordScorer::Request req;req.tuningOffsets.assign(6,0);req.minHitRatio=1;req.pitchCheckCents=30;
    ChordScorer::Note a;a.string=0;a.fret=12;a.whammy=bar(-4);a.sustain=2;
    ChordScorer::Note b=a;b.string=2;b.fret=13;req.notes={a,b};
    for(const std::vector<double> played: {std::vector<double>{52,63},{48,59},{48,63},{50,61},{}}) {
        auto samples=signal(played);auto result=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
        bool expected=played==std::vector<double>{52,63} || played==std::vector<double>{48,59};
        if(result.isHit!=expected) {std::cerr<<"coherent chord failed\n";return 1;} ++checks;
    }
    auto w=bar(-4);w.segments[0].curve={{0,0},{1,-4},{2,0}};
    if(w.pitch(.5)!=-2 || w.pitch(1.5)!=-2 || w.pitch(3)!=0 || !w.valid(2))return 1;
    w.segments[0].curve[1].t=3;if(w.valid(2))return 1;
    // Capability exclusions are identical for silence and a correct signal;
    // an all-visual chart never yields an automatic perfect score.
    a.whammy=bar(0);a.whammy.segments[0].curve.clear();a.whammy.segments[0].vibrato="wide";
    req.notes={a};
    for(bool silence:{false,true}) {
        auto samples=signal(silence?std::vector<double>{}:std::vector<double>{52});
        auto r=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
        if(r.totalStrings!=0 || r.isHit || r.score!=0 || r.results[0].exclusionReason!="bar_vibrato_without_exact_pitch")return 1;
    }
    // Exercise the actual threaded verifier, including a backwards seek. No
    // physical device is used; the ring supplies a known narrow-band signal.
    for(double rate:{.5,1.,1.5}) for(double played:{52.,48.,50.}) {
        FixtureRing ring(played);NoteVerifier verifier(ring);
        NoteVerifier::ChartUpdate chart;chart.tuningOffsets.assign(6,0);chart.pitchCheckCents=30;
        NoteVerifier::ChartNote n;n.id="bar-attack";n.t=1;n.string=0;n.fret=12;n.sus=2;n.whammy=bar(-4);
        chart.notes={n};verifier.setChart(chart);verifier.prepare(48000,512);
        for(int pass=0;pass<2;pass++) {
            if(pass) {verifier.setPlayhead(0,true,rate);juce::Thread::sleep(35);}
            verifier.setPlayhead(1,true,rate);juce::Thread::sleep(40);
            verifier.setPlayhead(1.3,true,rate);juce::Thread::sleep(35);
            const auto v=verifier.drainVerdicts();
            if(v.size()!=1 || v[0].id!=n.id || v[0].detected!=(played!=50.)) {
                std::cerr<<"verifier mismatch rate "<<rate<<" played "<<played<<" pass "<<pass<<" count "<<v.size()<<"\n";return 1;
            }
            if(!verifier.drainVerdicts().empty())return 1;
            ++checks;
        }
        verifier.stop();
    }
    std::cout<<checks<<" whammy signal checks passed\n";
}
