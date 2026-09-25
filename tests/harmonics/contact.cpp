#include "ChordScorer.h"
#include "NoteVerifier.h"
#include "InputRingReader.h"
#include <iostream>
#include <stdexcept>

static void require(bool ok) {if(!ok)throw std::runtime_error("Delayed harmonic contact check failed");}
static HarmonicContact contact() {
    HarmonicContact h;h.enabled=true;h.start=.15;h.end=2;
    h.target={"artificial",7,19,"harmonic"};h.sourceId="source:tie";return h;
}
class Ring final:public InputRingReader {
public:
    explicit Ring(double partial):samples(16384) {
        const double hz=440*std::pow(2.,(54.-69)/12)*partial;
        for(size_t i=0;i<samples.size();++i)samples[i]=(float)(.3*std::sin(2*3.141592653589793*hz*i/48000)
            +.03*std::sin(4*3.141592653589793*hz*i/48000));
    }
    std::vector<float> getInputFrame(int) const override{return samples;}
    uint64_t getInputSince(uint64_t i,std::vector<float>& out)const override{out.clear();return i;}
    double getCurrentSampleRate()const override{return 48000;}
    std::vector<float> samples;
};
int main() {
    ChordScorer scorer;ChordScorer::Request req;req.tuningOffsets.assign(6,0);req.pitchCheckCents=30;req.harmonicVerify=true;
    ChordScorer::Note n;n.fret=14;n.sustain=2;n.harmonicContact=contact();
    require(n.harmonicContact.valid(2,14));require(n.harmonicContact.attackEnd(.2,1)==.15);
    for(double elapsed:{.05,.149,.15,.2,1.})for(double partial:{1.,2.,3.}) {
        n.elapsed=elapsed;req.notes={n};Ring ring(partial);
        auto result=scorer.scoreChord(ring.samples.data(),(int)ring.samples.size(),48000,req);
        require(result.isHit==(elapsed<.15 && partial==1));require(result.totalStrings==1);
    }
    // The actual threaded queue finalizes once, at the contact boundary.
    // A seek into the harmonic cannot reopen the ordinary attack's late grace.
    for(double rate:{.5,1.,1.5})for(bool late:{false,true})for(double partial:{1.,3.}) {
        Ring ring(partial);NoteVerifier verifier(ring);
        NoteVerifier::ChartUpdate chart;chart.tuningOffsets.assign(6,0);chart.pitchCheckCents=30;
        NoteVerifier::ChartNote cn;cn.id="one-pick";cn.t=1;cn.fret=14;cn.sus=2;cn.harmonicContact=contact();
        chart.notes={cn};verifier.setChart(chart);verifier.prepare(48000,512);
        for(int pass=0;pass<2;++pass) {
            if(pass){verifier.setPlayhead(0,true,rate);juce::Thread::sleep(35);}
            verifier.setPlayhead(late?1.16:1.,true,rate);juce::Thread::sleep(35);
            verifier.setPlayhead(1.4,true,rate);juce::Thread::sleep(35);
            auto verdict=verifier.drainVerdicts();
            require(verdict.size()==1);require(verdict[0].detected==(!late && partial==1));
            require(verifier.drainVerdicts().empty());
        }
        verifier.stop();
    }
    std::cout<<"Contact signal, single-attack, seek and speed checks passed\n";
}
