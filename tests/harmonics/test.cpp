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
    struct Signal { std::vector<std::pair<int,double>> partials; int accepts; };
    // accepts: 0 never, 1 every policy, 2 semi/feedback, 3 feedback only.
    const std::vector<Signal> signals={
        {{{2,.4},{4,.08}},1}, {{{2,.2},{4,.4}},1}, {{{1,.4},{2,.3},{4,.05}},2},
        {{{1,.4},{2,.04},{3,.02}},3}, {{{1,.4},{2,.22},{3,.18},{4,.12}},3},
        {{{3,.4},{6,.08}},0}, {{},0}};
    for (bool bass : {false,true}) for (int capo : {0,2}) for (int drop : {0,-2})
    for (const std::string kind : {"pinch","artificial","tapped","semi","feedback"}) {
        ChordScorer::Request req; req.arrangement=bass?"bass":"guitar"; req.stringCount=bass?4:6;
        req.tuningOffsets.assign(req.stringCount,drop);req.capo=capo;req.pitchCheckCents=50;req.minHitRatio=1;
        ChordScorer::Note n; n.string=0;n.fret=7;
        n.harmonicTarget={kind,12,12,kind=="semi"?"mixed":kind=="feedback"?"attack_either":"harmonic"};
        req.notes={n};
        const double hz=440*std::pow(2.0,((bass?28:40)+capo+drop+7-69)/12.0);
        for (const auto& signal : signals) {
            std::vector<float> samples(16800);
            for (size_t i=0;i<samples.size();++i) for (const auto& [p,amp] : signal.partials)
                samples[i]+=(float)(amp*std::sin(2*3.141592653589793*hz*p*i/48000));
            const bool expected=signal.accepts==1 || (signal.accepts==2 && (kind=="semi"||kind=="feedback"))
                || (signal.accepts==3 && kind=="feedback");
            const auto result=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
            if (result.isHit!=expected || result.totalStrings!=1 || result.results.size()!=1 || result.results[0].fret!=7) {
                std::cerr<<"Fretted harmonic mismatch "<<kind<<" signal="<<signal.accepts<<" bass="<<bass<<" capo="<<capo<<" drop="<<drop<<"\n";return 1;
            }
            ++checks;
        }
    }
    const double nodes[]={12,7,5,4,3.2,14.7,2.4};
    for (int partial=2;partial<=8;++partial) for (int fret : {3,12,17}) {
        ChordScorer::Request req;req.tuningOffsets.assign(6,0);req.pitchCheckCents=20;req.minHitRatio=1;req.harmonicVerify=true;
        ChordScorer::Note n;n.string=0;n.fret=fret;n.harmonicTarget={"tapped",nodes[partial-2],offsets[partial-2],"harmonic"};
        ChordScorer::Note ordinary;ordinary.string=5;ordinary.fret=9;
        const double hz=partial*440*std::pow(2.0,(40+fret-69)/12.0);
        const double otherHz=440*std::pow(2.0,(64+9-69)/12.0);
        for(bool chord : {false,true}) {
            req.notes=chord?std::vector<ChordScorer::Note>{n,ordinary}:std::vector<ChordScorer::Note>{n};
            std::vector<float> samples(16800);
            for(size_t i=0;i<samples.size();++i) samples[i]=(float)(.3*std::sin(2*3.141592653589793*hz*i/48000)
                +(chord?.15*std::sin(2*3.141592653589793*otherHz*i/48000):0));
            auto result=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
            if(!result.results[0].hit || result.totalStrings!=(chord?2:1)) {
                std::cerr<<"Fretted partial/chord rejected "<<partial<<" fret "<<fret<<" chord "<<chord<<"\n";return 1;
            }
            if(chord) {
                req.notes={ordinary};
                const auto legacy=scorer.scoreChord(samples.data(),(int)samples.size(),48000,req);
                if(result.results[1].hit!=legacy.results[0].hit) throw std::runtime_error("Ordinary chord scoring changed");
            }
            ++checks;
        }
    }
    std::cout<<checks<<" harmonic pitch/policy checks passed\n";
}
