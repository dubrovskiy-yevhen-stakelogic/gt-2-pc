#include "game/audio/frame_clock.h"
#include "game/audio/mixer.h"
#include "game/audio/sequencer.h"
#include <iostream>
#include <stdexcept>

struct Driver:gt2::audio::NullVoiceDriver {
    std::atomic<int> starts{0},releases{0};
    void Play(int8_t* handle,const gt2::audio::VoiceRequest&) override { *handle=0;++starts; }
    void Release(int8_t*) override { ++releases; }
    void Detach(int8_t* handle) override { *handle=-1; }
};
void Check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
    using namespace std::chrono_literals;
    try {
        Driver driver;
        std::array<uint16_t,384> pitch;pitch.fill(4096);
        std::array<uint16_t,128> pan,velocity;pan.fill(16384);velocity.fill(16384);
        gt2::audio::Sequencer sequencer(driver,{pitch.data(),pan.data(),velocity.data()});
        gt2::SeqgFile file;file.bytes={0,0,uint8_t(0x80|60),100,4,120,0};
        gt2::SeqgSequence sequence;sequence.tempo=gt2::audio::Sequencer::kIncrementPerField;sequence.tracks[0]=1;
        file.sequences.push_back(sequence);
        gt2::InstBank bank;gt2::InstSample sample{};sample.volume=16384;sample.baseNote=60*256;sample.byte09=64;sample.byte0D=127;
        bank.samples.push_back(sample);
        const std::vector<gt2::InstProgram> programs{{64,16384,{0}}};
        sequencer.Start(file,0,bank,programs,16384);
        gt2::audio::AudioFrameClock clock;std::atomic<int> ticks{0};
        clock.Start([&]{sequencer.Tick();++ticks;});
        // No render/view Frame calls while a synthetic model load blocks. Poll so a loaded
        // CI runner cannot fail the check by scheduling the clock thread late.
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while((driver.starts.load()!=1 || driver.releases.load()!=1) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(5ms);
        Check(driver.starts==1 && driver.releases==1,"render stall stretched a four-tick music note");
        {
            gt2::audio::ScopedMixPause pause;
            std::this_thread::sleep_for(40ms);const auto before=ticks.load();
            std::this_thread::sleep_for(100ms);
            Check(ticks==before,"settings pause advanced the music sequence");
        }
        std::this_thread::sleep_for(60ms);
        clock.Stop();const auto stopped=ticks.load();std::this_thread::sleep_for(30ms);
        Check(ticks==stopped,"audio clock outlived its owner");
        { // A one-shot voice ends by itself; its owner slot must still be detachable (sequencer slot leaks)
            gt2::audio::Mixer mixer;
            const uint8_t endBlock[16]={0,1}; // one-shot: the first block ends the sample
            Check(mixer.Upload(0,endBlock),"sample upload");
            gt2::audio::VoiceRequest r;r.address=0;
            int8_t handle=-1;mixer.Play(&handle,r);
            Check(handle>=0,"voice allocated");
            float out[441*2];mixer.Mix(out,441);
            mixer.Detach(&handle);
            Check(handle==-1,"Detach clears a slot whose voice already ended");
        }
        { // A track restarted immediately after Stop() must not drop its first notes on the old slots
            Driver restartDriver;
            gt2::audio::Sequencer restart(restartDriver,{pitch.data(),pan.data(),velocity.data()});
            gt2::SeqgFile longFile;longFile.bytes={0,0,uint8_t(0x80|60),100,0x87,0x68,0}; // one 1000-tick note
            gt2::SeqgSequence longSequence;longSequence.tempo=gt2::audio::Sequencer::kIncrementPerField;longSequence.tracks[0]=1;
            longFile.sequences.push_back(longSequence);
            gt2::InstBank wideBank; // one note starts all 25 slots (one sample per program entry, all in range)
            gt2::InstSample wideSample{};wideSample.volume=16384;wideSample.baseNote=60*256;wideSample.note=0;wideSample.byte0D=127;
            for(int i=0;i<gt2::audio::Sequencer::kSlots;i++)wideBank.samples.push_back(wideSample);
            const std::vector<gt2::InstProgram> widePrograms{{64,16384,std::vector<uint8_t>(gt2::audio::Sequencer::kSlots,0)}};
            restart.Start(longFile,0,wideBank,widePrograms,16384);
            restart.Tick();
            Check(restartDriver.starts==gt2::audio::Sequencer::kSlots,"all slots busy");
            restart.Stop();
            restart.Start(longFile,0,wideBank,widePrograms,16384);
            restart.Tick();
            Check(restartDriver.starts==2*gt2::audio::Sequencer::kSlots,"restart reuses a stopped player's slots");
        }
        { // A short note whose event lands late in a 4-tick field must still sound for that field
            Driver lateDriver;
            gt2::audio::Sequencer late(lateDriver,{pitch.data(),pan.data(),velocity.data()});
            gt2::SeqgFile lateFile;lateFile.bytes={1,0,uint8_t(0x80|60),100,2,0}; // delta 1, note length 2
            gt2::SeqgSequence lateSequence;lateSequence.tempo=2000000;lateSequence.tracks[0]=1;
            lateFile.sequences.push_back(lateSequence);
            late.Start(lateFile,0,bank,programs,16384);
            late.Tick();
            Check(lateDriver.releases==0,"a note starting late in the field is not released at once");
            late.Tick();
            Check(lateDriver.releases==1,"the short note is released on the next field");
        }
        std::cout<<"Music note released during blocked render; settings pause, shutdown and slot detach passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
