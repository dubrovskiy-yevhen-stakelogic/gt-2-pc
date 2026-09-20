#include "game/audio/race_audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "game/camera/race_camera.h"
#include "game/sim/trig.h"

namespace gt2::audio {
namespace {

constexpr uint32_t kPitchTableAddress = 0x8008FDB8u; // 384 x u16 (0x8007A52C)
constexpr uint32_t kPanTableAddress = 0x800901B8u;   // 128 x u16 (0x8007A170)
constexpr uint32_t kEffectSamplesAddress = 0x8002F528u; // overlay: 6 sample indices (FUN_80018760, 0x80018138)
constexpr uint32_t kLightSamplesAddress = 0x8002F530u;  // overlay: 4 sample indices (0x800189C4)
constexpr uint32_t kAiBankToken = 0x80000000u;
// The reverb the boot code (EXE 0x80010E14) sets up: 0x80079EF8(4) copies preset 4 of the table 0x80092EA4 (0x40 bytes =
// the 32 registers 0x1F801DC0..) and mBASE from 0x80092E90 + 4 * 2; 0x8007A0E0(1) sets SPU control bit 7; 0x8007A104
// (0x0FFF, 0x0FFF) the output volumes 0x1F801D84 / 0x1F801D86 (immediates of the call at 0x80010EA8).
constexpr uint32_t kReverbPresetTable = 0x80092EA4u, kReverbBaseTable = 0x80092E90u, kReverbPreset = 4;
constexpr int16_t kReverbDepth = 0x0FFF;

int32_t ToFixed16(float metres) { return int32_t(std::lround(double(metres) * 65536.0)); }

// 0x80081164: the unit vector (4096 = 1.0, s16) and the length (16.16) of a 16.16 vector (the GTE normalisation, ported in
// the camera library).
int32_t Normalise(const int32_t v[3], int32_t out[3]) {
    int16_t direction[3];
    const int32_t length = camera::VectorLength(direction, v);
    for (int i = 0; i < 3; i++) out[i] = direction[i];
    return length;
}

// 0x80081500: GTE MVMVA (sf = 1, lm = 0) of two s16 vectors - IR1 = (a . b) >> 12 saturated to s16.
int32_t Dot(const int32_t a[3], const int32_t b[3]) {
    const int64_t mac = (int64_t(a[0]) * b[0] + int64_t(a[1]) * b[1] + int64_t(a[2]) * b[2]) >> 12;
    return int32_t(std::clamp<int64_t>(mac, -0x8000, 0x7FFF));
}

void UnitOf(const float v[3], int32_t out[3]) {
    const double l = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    for (int i = 0; i < 3; i++) out[i] = l < 1e-6 ? 0 : int32_t(std::lround(v[i] / l * 4096.0));
}

} // namespace

RaceAudio::RaceAudio() : driver_(mixer_) {
    context_.driver = &driver_;
    context_.user = this;
    context_.layers = &RaceAudio::LayersOf;
    context_.sample = &RaceAudio::SampleOf;
    context_.effect = &RaceAudio::EffectOf;
}

RaceAudio::~RaceAudio() { device_.Close(); }

uint32_t RaceAudio::Upload(std::span<const uint8_t> data) {
    const uint32_t address = nextAddress_;
    if (!mixer_.Upload(address, data)) throw std::runtime_error("sound: the banks do not fit the 512 KB sample RAM");
    nextAddress_ = (address + uint32_t(data.size()) + 15) & ~15u;
    return address;
}

int RaceAudio::LoadEngineBank(const GtfsVolume& vol, const std::string& path) {
    Bank b;
    b.bank = ParseEngineBank(vol.Read(path));
    b.address = Upload(b.bank.data);
    for (EngineLayer& l : b.bank.layers) l.sampleAddress += b.address; // 0x80079078: the data's SPU address is added
    banks_.push_back(std::move(b));
    return int(banks_.size() - 1);
}

const EngineLayer* RaceAudio::LayersOf(void* user, uint32_t token) {
    RaceAudio& self = *static_cast<RaceAudio*>(user);
    return self.banks_[token & 0xFFFF].bank.layers.data();
}

const InstSample* RaceAudio::SampleOf(void* user, uint32_t token) {
    RaceAudio& self = *static_cast<RaceAudio*>(user);
    return &self.effects_.samples[token];
}

const InstSample* RaceAudio::EffectOf(void* user, uint32_t index) {
    RaceAudio& self = *static_cast<RaceAudio*>(user);
    return index < self.effects_.samples.size() ? &self.effects_.samples[index] : nullptr;
}

void RaceAudio::Load(const GtfsVolume& vol, const GuestImage& exe, const GuestImage& overlay, std::span<const CarSoundSetup> cars) {
    banks_.clear();
    cars_.clear();
    controlClass_.clear();
    previousPosition_.clear();
    primed_.clear();
    view_.clear();
    twoPlayerPrimed_ = false;
    nextAddress_ = 0x1000;
    // Tables of the executable and the race overlay.
    sinTable_.assign(sim::SinTable().begin(), sim::SinTable().end());
    pitchTable_.resize(384);
    const uint32_t pitchTable = exe.Sim(kPitchTableAddress), panTable = exe.Sim(kPanTableAddress); // this build's tables (exe_profile.h)
    for (uint32_t i = 0; i < 384; i++) pitchTable_[i] = exe.Get<uint16_t>(pitchTable + i * 2);
    panTable_.resize(128);
    for (uint32_t i = 0; i < 128; i++) panTable_[i] = exe.Get<uint16_t>(panTable + i * 2);
    context_.sinTable = sinTable_.data();
    context_.pitchTable = pitchTable_.data();
    context_.panTable = panTable_.data();
    const uint32_t effectSamples = overlay.Sim(kEffectSamplesAddress), lightSamples = overlay.Sim(kLightSamplesAddress);
    for (uint32_t i = 0; i < 6; i++) context_.effectSamples[i] = overlay.Get<uint8_t>(effectSamples + i);
    for (uint32_t i = 0; i < 4; i++) context_.lightSamples[i] = overlay.Get<uint8_t>(lightSamples + i);
    mixer_.SetReverb(GameReverb(exe));
    // The race effect bank (0x8017D894 -> sound/se01.ins).
    effects_ = ParseInstBank(vol.Read("sound/se01.ins"));
    const uint32_t effectAddress = Upload(effects_.data);
    for (InstSample& s : effects_.samples) s.address = uint16_t(s.address + (effectAddress >> 3)); // 0x80078974
    for (uint8_t index : context_.effectSamples)
        if (index >= effects_.samples.size()) throw std::runtime_error("sound: effect sample index outside sound/se01.ins");
    // The cars' banks (0x8001882C: the players' intake + exhaust sets, the AI's shared exhaust set).
    int aiBank[2] = {-1, -1};
    const uint32_t effectTokens[4] = {context_.effectSamples[0], context_.effectSamples[1], context_.effectSamples[2], context_.effectSamples[3]};
    for (const CarSoundSetup& car : cars) {
        CarSound sound{};
        if (car.controlClass >= 2) {
            const int engine = LoadEngineBank(vol, EngineSoundPath(car.soundId));
            const int exhaust = LoadEngineBank(vol, ExhaustSoundPath(car.soundId, car.exhaustByte));
            InitCarSound(context_, sound, 0, uint32_t(engine), uint8_t(banks_[size_t(engine)].bank.layers.size()), uint32_t(exhaust),
                         uint8_t(banks_[size_t(exhaust)].bank.layers.size()), effectTokens);
        } else {
            int& bank = aiBank[car.turbo ? 1 : 0];
            if (bank < 0) bank = LoadEngineBank(vol, AiExhaustSoundPath(car.turbo));
            InitCarSound(context_, sound, 1, uint32_t(bank), uint8_t(banks_[size_t(bank)].bank.layers.size()), 0, 0, effectTokens);
        }
        cars_.push_back(sound);
        controlClass_.push_back(car.controlClass);
        previousPosition_.push_back({0, 0, 0});
        primed_.push_back(0);
        view_.push_back(CarView{});
    }
    loaded_ = true;
}

bool RaceAudio::OpenDevice(std::string& error) { return device_.Open(mixer_, error); }

ReverbSettings GameReverb(const GuestImage& exe) {
    ReverbSettings r;
    const uint32_t presets = exe.Sim(kReverbPresetTable), bases = exe.Sim(kReverbBaseTable);
    for (uint32_t i = 0; i < 32; i++) r.registers[i] = exe.Get<uint16_t>(presets + kReverbPreset * 0x40 + i * 2);
    r.base = exe.Get<uint16_t>(bases + kReverbPreset * 2);
    r.outLeft = r.outRight = kReverbDepth;
    r.enabled = true;
    return r;
}

void RaceAudio::Step(const sim::RaceSim& race, const Listener& listener, bool attract) {
    if (!loaded_) return;
    mixer_.Poll();
    const size_t count = std::min(cars_.size(), race.CarCount());
    int32_t listenerPosition[3], listenerVelocity[3], listenerDir[3], axisSide[3], axisRight[3];
    for (int i = 0; i < 3; i++) { listenerPosition[i] = ToFixed16(listener.position[i]); listenerVelocity[i] = ToFixed16(listener.velocity[i]); }
    const int32_t listenerSpeed = Normalise(listenerVelocity, listenerDir);
    UnitOf(listener.axisSide, axisSide);
    UnitOf(listener.axisRight, axisRight);
    // 0x800140A4 per car: the relative vector, the distance volume; the cars are then visited nearest first.
    struct Entry { size_t car; int32_t distance, volume, relative[3], velocity[3]; };
    std::vector<Entry> entries;
    for (size_t car = 0; car < count; car++) {
        const sim::CarPose pose = race.Pose(car);
        Entry e{car, 0, 0, {}, {}};
        for (int i = 0; i < 3; i++) {
            e.relative[i] = pose.worldPosition[size_t(i)] - listenerPosition[i];
        }
        CarVelocity(race, car, e.velocity);
        e.volume = DistanceVolume(e.relative, controlClass_[car] < 2, attract, &e.distance);
        entries.push_back(e);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.distance < b.distance; });
    for (size_t rank = 0; rank < entries.size(); rank++) {
        const Entry& e = entries[rank];
        CarSoundInputs in;
        in.distanceVolume = e.volume;
        in.muted = false;
        in.rank = int32_t(rank) + 1;
        in.focused = int(e.car) == listener.focusedCar;
        in.controlClass = controlClass_[e.car];
        in.attract = attract;
        in.inCarView = listener.inCarView;
        in.inCarViewAlt = listener.inCarViewAlt;
        in.rate = 30;
        int32_t dir[3], carDir[3];
        Normalise(e.relative, dir);
        const int32_t carSpeed = Normalise(e.velocity, carDir);
        in.panSide = Dot(dir, axisSide);
        in.pan = Dot(dir, axisRight);
        in.doppler = DopplerFactor(kSpeedOfSound, listenerSpeed, Dot(dir, listenerDir), carSpeed, Dot(dir, carDir));
        in.body = reinterpret_cast<const uint8_t*>(&race.CarAt(e.car).body);
        UpdateCarSound(context_, cars_[e.car], in);
    }
}

void RaceAudio::Prime(const sim::RaceSim& race) {
    for (size_t car = 0; car < previousPosition_.size() && car < race.CarCount(); car++) {
        previousPosition_[car] = race.Pose(car).worldPosition;
        primed_[car] = 1;
    }
}

// car + 0x868 as 0x800133F0 writes it: car + 0x85C = the last frame's car + 0x830 (the position WITH the reference offset of
// the CG, (0, 0, car + 0x5A) through the matrix >> 8), then car + 0x868 = the new position from 0x8001336C BEFORE that offset
// is added minus car + 0x85C; so a standing car has the negated offset as its velocity (the start hold's doppler).
void RaceAudio::CarVelocity(const sim::RaceSim& race, size_t car, int32_t out[3]) {
    const sim::CarPose pose = race.Pose(car);
    if (!primed_[car]) { previousPosition_[car] = pose.worldPosition; primed_[car] = 1; } // no Prime: the first frame's own position
    const int32_t cgOffset = race.CarAt(car).body.cgOffset;
    for (size_t i = 0; i < 3; i++) {
        const int32_t unshifted = int32_t(uint32_t(pose.worldPosition[i]) - uint32_t((int32_t(pose.rotation[i][2]) * cgOffset) >> 8));
        out[i] = int32_t(uint32_t(unshifted) - uint32_t(previousPosition_[car][i]));
        previousPosition_[car][i] = pose.worldPosition[i];
    }
}

// 0x800140A4 for one car in a drawn view (camera = the view's camera object): car + 0x80C = car + 0x830 - camera + 0xB8, car
// + 0x804 the distance estimate, car + 0x808 the distance volume.
void RaceAudio::ViewCar(const sim::RaceSim& race, size_t car, const Listener& camera, bool attract) {
    CarView& v = view_[car];
    const sim::CarPose pose = race.Pose(car);
    for (size_t i = 0; i < 3; i++) {
        const int32_t eye = camera.cameraMotion ? camera.cameraPosition[i] : ToFixed16(camera.position[i]);
        v.relative[i] = int32_t(uint32_t(pose.worldPosition[i]) - uint32_t(eye));
    }
    v.volume = DistanceVolume(v.relative, controlClass_[car] < 2, attract, &v.distance);
}

// 0x800146D8 for one car: `view` = what the last drawn frame left in its record (car + 0x80C / + 0x808 / + 0x868), `listener`
// = the camera object of its mix (pan axis, motion, followed car, in-car flags); `rank` = the argument + 1 (car + 0xAA5).
void RaceAudio::MixCar(const sim::RaceSim& race, size_t car, const CarView& view, const Listener& listener, int32_t rank, bool attract) {
    int32_t listenerVelocity[3], listenerDir[3], axisSide[3], axisRight[3];
    for (int i = 0; i < 3; i++) listenerVelocity[i] = ToFixed16(listener.velocity[i]);
    int32_t listenerSpeed = Normalise(listenerVelocity, listenerDir);
    UnitOf(listener.axisSide, axisSide);
    UnitOf(listener.axisRight, axisRight);
    if (listener.cameraMotion) { // camera + 0xFC / + 0xE4 / + 0xEC / + 0xF4 as 0x800146D8 reads them
        listenerSpeed = listener.cameraSpeed;
        for (int i = 0; i < 3; i++) {
            listenerDir[i] = listener.cameraDirection[i];
            axisSide[i] = listener.cameraAxisSide[i];
            axisRight[i] = listener.cameraAxisRight[i];
        }
    }
    CarSoundInputs in;
    in.distanceVolume = view.volume;
    in.muted = false;
    in.rank = rank;
    in.focused = int(car) == listener.focusedCar;
    in.controlClass = controlClass_[car];
    in.attract = attract;
    in.inCarView = listener.inCarView;
    in.inCarViewAlt = listener.inCarViewAlt;
    in.rate = 30;
    int32_t dir[3], carDir[3];
    Normalise(view.relative, dir);
    const int32_t carSpeed = Normalise(view.velocity, carDir);
    in.panSide = Dot(dir, axisSide);
    in.pan = Dot(dir, axisRight);
    in.doppler = DopplerFactor(kSpeedOfSound, listenerSpeed, Dot(dir, listenerDir), carSpeed, Dot(dir, carDir));
    in.body = reinterpret_cast<const uint8_t*>(&race.CarAt(car).body);
    UpdateCarSound(context_, cars_[car], in);
}

void RaceAudio::PrimeTwoPlayer(const sim::RaceSim& race, bool split) {
    Prime(race);
    for (CarView& v : view_) v = CarView{};
    lastSplit_ = split;
    twoPlayerPrimed_ = true;
}

void RaceAudio::StepTwoPlayer(const sim::RaceSim& race, const std::array<Listener, 2>& cameras, bool split, bool attract) {
    if (!loaded_) return;
    mixer_.Poll();
    if (!twoPlayerPrimed_) PrimeTwoPlayer(race, split);
    const size_t count = std::min(cars_.size(), race.CarCount());
    // The sound pass (Sim 0x80015DF4): this frame's camera objects, the view values and split flag of the previous frame's draw.
    if (lastSplit_) { // 0x80014ED0: the two players' cars, car i with rank 0 against camera i
        for (size_t car = 0; car < 2 && car < count; car++) MixCar(race, car, view_[car], cameras[car], 1, attract);
    } else { // 0x80014E6C: every car against camera 1, nearest first (the list 0x800140A4 built, ties in car order)
        std::vector<size_t> order(count);
        for (size_t car = 0; car < count; car++) order[car] = car;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return view_[a].distance < view_[b].distance; });
        for (size_t rank = 0; rank < order.size(); rank++) MixCar(race, order[rank], view_[order[rank]], cameras[0], int32_t(rank) + 1, attract);
    }
    // The frame's draw: 0x800133F0 per car, then 0x800140A4 per car and drawn view; split, 0x800297F4 keeps car 0's vector and
    // volume of player 1's view across player 2's (Sim 0x80029888..0x800298DC): car i against camera i.
    for (size_t car = 0; car < count; car++) CarVelocity(race, car, view_[car].velocity);
    for (size_t car = 0; car < count; car++) ViewCar(race, car, cameras[split ? std::min<size_t>(car, 1) : 0], attract);
    if (split && count > 1) { // car 0's distance estimate car + 0x804 is not restored: player 2's pass leaves its own
        const CarView kept = view_[0];
        ViewCar(race, 0, cameras[1], attract);
        const int32_t distance = view_[0].distance;
        view_[0] = kept;
        view_[0].distance = distance;
    }
    lastSplit_ = split;
}

void RaceAudio::StartLight(int32_t stage) {
    if (loaded_ && stage >= 0 && stage < 2) StartLightSound(context_, stage);
}

} // namespace gt2::audio
