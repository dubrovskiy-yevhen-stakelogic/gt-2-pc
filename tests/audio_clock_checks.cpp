#include "game/audio/frame_clock.h"
#include "game/audio/sequencer.h"
#include <iostream>
#include <stdexcept>

struct Driver:gt2::audio::NullVoiceDriver {
    std::atomic<int> starts{0},releases{0};
    void Play(int8_t* handle,const gt2::audio::VoiceRequest&) override { *handle=0;++starts; }
    void Release(int8_t*) override { ++releases; }
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
        // No render/view Frame calls while a synthetic model load blocks.
        std::this_thread::sleep_for(300ms);
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
        std::cout<<"Music note released during blocked render; settings pause and shutdown passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
