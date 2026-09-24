#include "ChordScorer.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

int main() {
    ChordScorer scorer;
    int checks=0;
    for (const auto bass : {false,true}) for (int capo : {0,2}) for (int drop : {0,-2}) {
        ChordScorer::Request req;
        req.arrangement=bass ? "bass" : "guitar"; req.stringCount=bass ? 4 : 6;
        req.tuningOffsets.assign(req.stringCount,drop); req.capo=capo;
        req.pitchCheckCents=50; req.harmonicVerify=true; req.minHitRatio=1;
        ChordScorer::Note n; n.string=0; n.fret=3; n.harmonic=true; n.harmonicSemitones=31;
        req.notes={n};
        for (int played : {31,34,3,19,43,-1}) for (bool comb : {true,false}) {
            req.harmonicVerify=comb;
            const int midi=(bass ? 28 : 40)+drop+capo+played;
            const double hz=440*std::pow(2.0,(midi-69)/12.0);
            std::vector<float> samples(16384);
            for (size_t i=0;i<samples.size();i++) {
                const double t=i/48000.0;
                samples[i]=played<0 ? 0.0f : (float)(.2*std::sin(2*3.141592653589793*hz*t)
                    +.04*std::sin(4*3.141592653589793*hz*t));
            }
            const auto result=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
            if (result.isHit != (played==31)) {
                std::cerr << "Wrong result: bass="<<bass<<" capo="<<capo<<" drop="<<drop<<" played="<<played<<" comb="<<comb<<"\n";
                return 1;
            }
            if (result.results[0].fret!=3) throw std::runtime_error("Source identity changed");
            ++checks;
        }
    }
    // Physical partials, not equal-tempered synthesizer approximations. A
    // real seventh partial must pass a 20-cent gate without widening it.
    const int offsets[]={12,19,24,28,31,34,36};
    for (int partial=2; partial<=8; ++partial) for (int string=0; string<6; ++string) {
        ChordScorer::Request req; req.tuningOffsets.assign(6,0); req.pitchCheckCents=20;
        ChordScorer::Note n; n.string=string; n.fret=3; n.harmonic=true; n.harmonicSemitones=offsets[partial-2];
        req.notes={n};
        const int bases[]={40,45,50,55,59,64};
        const double hz=partial*440*std::pow(2.0,(bases[string]-69)/12.0);
        std::vector<float> samples(4096);
        for (size_t i=0;i<samples.size();++i) samples[i]=.2f*(float)std::sin(2*3.141592653589793*hz*i/48000);
        auto result=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
        if(!result.isHit) { std::cerr<<"Physical partial rejected: "<<partial<<" string "<<string<<"\n";return 1; }
        ++checks;
    }
    std::cout<<checks<<" natural harmonic pitch checks passed\n";
}
