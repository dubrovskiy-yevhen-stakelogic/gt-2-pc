#include "gt2formats/track.h"
#include "gt2formats/boot_images.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/str_video.h"
#include "gt2vfs/gtfs.h"
#include "game/arcade/arcade_menus.h"
#include "game/menu/menu_runtime.h"
#include "game/shell/title_attract.h"
#include "game/sim/ground.h"
#include "game/arcade/arcade_setup.h"
#include "game/pc_features.h"
#include "platform/input/dualsense.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/inflate.h"
#include <algorithm>
#include <sstream>
#include "game/audio/pause.h"
#include "game/audio/music_player.h"
#include "game/shell/title_options.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

static int checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }

static void CheckPcChanges(const char* discPath) {
    using namespace gt2;
    std::array<uint8_t, 64> usb{};
    usb[0] = 1;
    std::fill(usb.begin() + 1, usb.begin() + 5, uint8_t(128));
    usb[6] = 255; usb[8] = 0x28; usb[9] = 8; // R2 and Cross, no brake, centred sticks
    input::Ps1PadFrame pad;
    Check(input::DecodeDualSenseInput(usb, false, pad) && pad.pressureR2 == 255 && pad.pressureL2 == 0 &&
          pad.buttons == (input::ps1::kCross | input::ps1::kR2), "USB input maps accelerator and buttons");
    std::array<uint8_t, 78> bt{};
    bt[0] = 0x31;
    std::copy(usb.begin() + 1, usb.end(), bt.begin() + 2);
    uint32_t crc = 0xFFFFFFFF;
    for (int i = -1; i < 74; ++i) {
        const uint8_t byte = i < 0 ? 0xA1 : bt[size_t(i)];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool feedback = ((crc ^ (byte >> bit)) & 1) != 0;
            crc >>= 1; if (feedback) crc ^= 0xEDB88320;
        }
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) bt[74 + i] = uint8_t(crc >> (8 * i));
    Check(input::DecodeDualSenseInput(bt, true, pad) && pad.pressureR2 == 255 && pad.pressureL2 == 0 &&
          pad.analog == std::array<uint8_t, 4>{128,128,128,128}, "enhanced Bluetooth input retains accelerator and sticks");
    const auto goodPad = pad;
    bt[8] ^= 1;
    Check(!input::DecodeDualSenseInput(bt, true, pad) && pad.pressureR2 == goodPad.pressureR2, "bad Bluetooth CRC leaves input untouched");
    const std::array<uint8_t, 10> simple = {1,128,128,128,128,0x28,8,0,0,255};
    Check(input::DecodeDualSenseInput(simple, true, pad) && pad.pressureR2 == 255 && (pad.buttons & input::ps1::kCross), "simple Bluetooth input before effects start");
    Check(!input::DecodeDualSenseInput(std::span(usb).first(10), false, pad), "truncated USB input rejected");
    input::PadTables mappings;
    std::array<uint8_t, 0x200> object{};
    std::array<uint8_t, 20> calibration{};
    std::array<uint8_t, 44> tables{};
    tables.fill(31);
    const auto pedals = input::TriggerPedalTable(tables.data() + 11);
    std::copy(pedals.begin(), pedals.end(), tables.begin() + 11);
    input::InitTracker(object.data() + input::pad_object::kTracker, mappings);
    input::PollPad(object.data(), pad, mappings, calibration.data(), 2);
    input::BuildRaceLogical(object.data(), tables.data());
    auto word = [&](size_t at) { return unsigned(object[at]) | unsigned(object[at + 1]) << 8; };
    Check(word(input::pad_object::kValues + 4) == 255 && word(input::pad_object::kValues + 6) == 0 &&
          (word(input::pad_object::kAnalogFlags) & 12) == 12, "decoded Bluetooth R2 reaches native logical gas without brake");

    shell::GraphicsSettings graphics;
    graphics.Parse("render_height", "2160");
    shell::GraphicsSettings restored;
    std::istringstream settings(graphics.Serialize());
    std::string line;
    while (std::getline(settings, line)) { const auto at = line.find('='); if (at != std::string::npos) restored.Parse(line.substr(0, at), line.substr(at + 1)); }
    Check(restored == graphics && restored.renderHeight * 16 / 9 == 3840, "4K fixed resolution survives settings round trip");
    restored.Parse("render_height", "99999");
    Check(restored.renderHeight == 0, "invalid fixed resolution falls back to window scale");

    struct CounterStream final : audio::StreamSource {
        size_t cursor = 0;
        void MixStream(float* out, size_t frames) override { std::fill(out, out + frames * 2, 0.25f); cursor += frames; }
    } stream;
    auto mixer = std::make_unique<audio::Mixer>();
    mixer->SetStream(&stream);
    std::array<float, 64> output{};
    mixer->Mix(output.data(), 32);
    Check(stream.cursor == 32 && output[0] != 0, "stream plays before overlay");
    {
        audio::ScopedMixPause pause;
        output.fill(0);
        mixer->Mix(output.data(), 32);
        Check(stream.cursor == 32 && std::all_of(output.begin(), output.end(), [](float f) { return f == 0; }), "overlay stops output and stream advancement");
        { audio::ScopedMixPause nested; }
        mixer->Mix(output.data(), 32);
        Check(stream.cursor == 32, "nested pause cannot prematurely resume audio");
    }
    mixer->Mix(output.data(), 32);
    Check(stream.cursor == 64, "closing overlay resumes stream at its previous position");
    // A looping tyre/engine voice must freeze with its mixer, not keep repeating behind the pause menu.
    auto raceMixer = std::make_unique<audio::Mixer>();
    auto referenceMixer = std::make_unique<audio::Mixer>();
    std::array<uint8_t, 16> loop{};
    loop[1] = 7; // loop end + repeat + loop start
    for (size_t i = 2; i < loop.size(); ++i) loop[i] = uint8_t(i * 13);
    audio::VoiceRequest voice;
    voice.address = 0x200 / 8; voice.volumeLeft = voice.volumeRight = 0x3FFF;
    int8_t handle = -1, referenceHandle = -1;
    raceMixer->Upload(0x200, loop); referenceMixer->Upload(0x200, loop);
    raceMixer->Play(&handle, voice); referenceMixer->Play(&referenceHandle, voice);
    std::vector<float> mixed(4096), referenceMixed(4096);
    raceMixer->Mix(mixed.data(), 2048); referenceMixer->Mix(referenceMixed.data(), 2048);
    Check(mixed == referenceMixed && std::any_of(mixed.begin(), mixed.end(), [](float f) { return f != 0; }), "looping race voice plays before pause");
    CounterStream raceStream;
    raceMixer->SetStream(&raceStream);
    raceMixer->SetPaused(true);
    std::fill(mixed.begin(), mixed.end(), 0.0f);
    raceMixer->Mix(mixed.data(), 2048);
    Check(raceStream.cursor == 0 && std::all_of(mixed.begin(), mixed.end(), [](float f) { return f == 0; }), "race pause freezes voices and music together");
    { audio::ScopedMixPause overlay; }
    raceMixer->Mix(mixed.data(), 2048);
    Check(raceStream.cursor == 0 && std::all_of(mixed.begin(), mixed.end(), [](float f) { return f == 0; }), "closing overlay does not resume an already paused race");
    raceMixer->SetStream(nullptr);
    raceMixer->SetPaused(false);
    std::fill(referenceMixed.begin(), referenceMixed.end(), 0.0f);
    raceMixer->Mix(mixed.data(), 2048); referenceMixer->Mix(referenceMixed.data(), 2048);
    Check(mixed == referenceMixed && raceMixer->ActiveVoices() == 1, "looping race voice resumes sample-identically");
    if (discPath) {
        DiscImage disc(discPath);
        const auto exe = LoadExeImage(disc);
        GtfsVolume vol(disc);
        Check(!shell::AttractDemoFilePath(LoadOverlayImage(disc, 1), vol, 1).empty(), "disc attract replay resolves");
        if (ProfileOf(exe).arcade) {
            const auto assets = arcade::ArcadeMenuAssets::Load(disc, vol);
            Check(assets.data.Text(0x800F8227) == "CAR SELECTION", "Arcade English text relocation");
            Check(assets.data.Text(0x800F845F).find("%d") != std::string::npos, "Arcade course length format");
            const auto movies = ReadStreamMovieTable(exe);
            const auto vlc = ReadGtVlcTable(LoadOverlayImage(disc, 5), kMovieVlcTableAddress);
            Check(vlc.entries == ReadGtVlcTable(LoadOverlayImage(disc, 2), kCourseMovieVlcTableAddress).entries,
                  "course and movie AC tables match");
            GtMovieReader reader(disc, StreamFileLba(disc), movies.at(5));
            GtMovieReader::Frame frame;
            Check(reader.NextFrame(frame), "course preview has a video frame");
            const auto codes = GtFrameToMdecCodes(frame.data.data(), frame.data.size(), vlc);
            Check(!DecodeMovieFrame(MdecCoreFromExe(exe), codes).rgb.empty(), "course preview decodes");
        } else {
            const auto data = menu::MenuData::Load(disc, vol);
            const auto pages = MenuPages::Load(vol);
            const auto assets = MenuAssets::Load(disc, vol);
            Check(pages.Count() > 0 && !pages.Page(0).items.empty(), "Simulation home page resolves");
            Check(!data.career.raceCarKinds.empty() && !assets.strings.empty(), "Simulation career and menu text load");
        }
        audio::MusicPlayer music, reference;
        music.Open(discPath, exe); reference.Open(discPath, exe);
        music.Play(0, true); reference.Play(0, true);
        std::vector<float> actual(88200), expected(88200);
        music.MixStream(actual.data(), 44100); reference.MixStream(expected.data(), 44100);
        Check(actual == expected && std::any_of(actual.begin(), actual.end(), [](float f) { return f != 0; }), "disc music reference plays non-silent audio");
        music.SetPaused(true);
        std::fill(actual.begin(), actual.end(), 0.0f);
        music.MixStream(actual.data(), 44100);
        Check(std::all_of(actual.begin(), actual.end(), [](float f) { return f == 0; }), "race music pause is silent");
        music.SetPaused(false);
        std::fill(expected.begin(), expected.end(), 0.0f);
        music.MixStream(actual.data(), 44100); reference.MixStream(expected.data(), 44100);
        Check(actual == expected, "race music resumes sample-identically at the paused position");
    }
}
int main(int argc, char** argv) {
    try {
        {
            // An indexed row validates nibble order and PS1 BGR555 conversion.
            std::vector<uint8_t> tim(66, 0);
            tim[0] = 0x10; tim[4] = 8; tim[8] = 44; tim[16] = 16; tim[18] = 1;
            tim[22] = 31; tim[25] = 0x7c;
            tim[52] = 14; tim[60] = 1; tim[62] = 1; tim[64] = 0x21;
            const auto picture = gt2::DecodeBootTim(tim);
            Check(picture.width == 4 && picture.height == 1 && picture.rgb[0] == 255 &&
                  picture.rgb[1] == 0 && picture.rgb[2] == 0 && picture.rgb[5] == 255,
                  "boot TIM palette and packed pixel order");
            tim.resize(65);
            bool rejectedTim = false;
            try { (void)gt2::DecodeBootTim(tim); } catch (const std::exception&) { rejectedTim = true; }
            Check(rejectedTim, "boot TIM rejects truncated pixels");
            if (argc > 1) {
                gt2::DiscImage disc(argv[1]);
                const auto exe = gt2::LoadExeImage(disc);
                for (const char* name : {"logo-scea.tim", "notice.tim"}) {
                    const auto image = gt2::LoadBootImage(exe, name);
                    Check(image.width == 640 && image.height == 512 && image.rgb.size() == 640 * 512 * 3,
                          "original boot artwork decodes from installed disc");
                }
            }
        }
        CheckPcChanges(argc > 1 ? argv[1] : nullptr);
        std::vector<uint8_t> paddedGzip = {0x1F,0x8B,8,0,0,0,0,0,0,3,1,3,0,0xFC,0xFF,'a','b','c',0xC2,0x41,0x24,0x35,3,0,0,0};
        paddedGzip.resize(2048, 0);
        Check(gt2::Gunzip(paddedGzip) == std::vector<uint8_t>({'a','b','c'}), "gzip trailer in padded VOL entry");
        paddedGzip[18] ^= 1;
        bool corruptRejected = false;
        try { (void)gt2::Gunzip(paddedGzip); } catch (const std::exception&) { corruptRejected = true; }
        Check(corruptRejected, "padded gzip must still validate CRC");
        // The rendered road must agree with collision height across positive/negative cell boundaries.
        for (int32_t height : {-8388609, -4194304, -1, 0, 4194303, 4194304, 8388608}) {
            gt2::TrackChunk c;
            c.centre = {0, height, 0};
            c.cellOrigin = {0, 0};
            c.road.vertices = {{0, 0, 640}, {64, 0, 640}, {64, 64, 640}, {0, 64, 640}};
            gt2::TrackPolygon p; p.vertex = {0, 1, 2, 3}; p.primCode = 0x28;
            const int32_t point[] = {32768, 0, 32768};
            const double physical = gt2::sim::InterpolateRoadHeight(c, p, point) / 65536.0;
            const auto rendered = gt2::TrackVertexToWorld(c, c.road.vertices[0]);
            Check(std::abs(rendered[1] - physical) < 0.0001, "rendered road does not match collision height");
        }
        using gt2::input::DualSenseReportTransport;
        using gt2::input::DualSenseTransport;
        Check(DualSenseReportTransport(64, 48) == DualSenseTransport::Usb, "USB report capabilities");
        Check(DualSenseReportTransport(78, 547) == DualSenseTransport::Bluetooth, "Bluetooth maximum report is longer than effect report");
        Check(DualSenseReportTransport(78, 48) == DualSenseTransport::Unsupported &&
              DualSenseReportTransport(12, 78) == DualSenseTransport::Unsupported &&
              DualSenseReportTransport(78, 65535) == DualSenseTransport::Unsupported, "reject invalid HID capabilities");
        const auto off = gt2::input::DualSenseReport({}, false, 0);
        Check(off[0] == 2 && off[3] == 0 && off[4] == 0 && off[11] == 5 && off[22] == 5, "USB effect release");
        const auto usb = gt2::input::DualSenseReport({1, 200, 8, 3}, false, 0);
        Check(usb[1] == 15 && usb[3] == 127 && usb[4] == 100, "PS1 motors to DualSense rumble");
        const auto halfRumble = gt2::input::DualSenseReport({1, 200, 8, 3, 50}, false, 0);
        const auto quarterRumble = gt2::input::DualSenseReport({1, 200, 8, 3, 25}, false, 0);
        const auto mutedRumble = gt2::input::DualSenseReport({1, 200, 8, 3, 0}, true, 0);
        Check(halfRumble[3] == 63 && halfRumble[4] == 50 && quarterRumble[3] == 31 && quarterRumble[4] == 25, "vibration strength scales both physical motors");
        Check(mutedRumble[5] == 0 && mutedRumble[6] == 0 && mutedRumble[13] == usb[11] && mutedRumble[24] == usb[22], "muting Bluetooth vibration preserves adaptive triggers");
        Check(usb[11] == 0x21 && usb[12] == 0xFC && usb[13] == 3 && usb[14] == 0xC0 && usb[17] == 0x3F, "accelerator zone mask/force");
        const auto bt = gt2::input::DualSenseReport({1, 200, 8, 3}, true, 31);
        Check(bt[0] == 0x31 && bt[1] == 0xF0 && bt[2] == 0x10, "Bluetooth header/sequence");
        Check(bt[13] == usb[11] && bt[24] == usb[22], "Bluetooth effect payload offset");
        // Independent bit-polynomial remainder including the HID output prefix.
        uint32_t remainder = 0xFFFFFFFF;
        for (int i = -1; i < 74; ++i) {
            const uint8_t byte = i < 0 ? 0xA2 : bt[size_t(i)];
            for (unsigned bit = 0; bit < 8; ++bit) {
                const bool feedback = ((remainder ^ (byte >> bit)) & 1) != 0;
                remainder >>= 1; if (feedback) remainder ^= 0xEDB88320;
            }
        }
        const uint32_t actual = uint32_t(bt[74]) | uint32_t(bt[75]) << 8 | uint32_t(bt[76]) << 16 | uint32_t(bt[77]) << 24;
        Check(actual == ~remainder, "Bluetooth CRC32");

        std::vector<uint8_t> career(0x7C9C);
        const auto original = career;
        Check(!gt2::arcade::TierOpen(career, 0), "new save should not have licence tier");
        gt2::pc::SetUnlocks(true, true);
        Check(gt2::arcade::TierOpen(career, 0), "course unlock override");
        gt2::ArcadeMenuData menu;
        menu.classes[0].cars.resize(3); menu.classes[6].cars.resize(5);
        const auto unlocked = gt2::arcade::ComputeClassUnlocks(menu, career);
        Check(unlocked.classSOpen && unlocked.classS.size() == 3 && unlocked.bonus.size() == 5, "car unlock overrides full roster");
        gt2::pc::SetUnlocks(false, false);
        Check(!gt2::arcade::TierOpen(career, 0) && career == original, "cheats are reversible without changing saves");

        const auto path = std::filesystem::temp_directory_path() / "gt2-invalid-disc-test.raw2352";
        { std::ofstream out(path, std::ios::binary); std::array<char, 2352> bytes{}; out.write(bytes.data(), bytes.size()); }
        bool rejected = false;
        try { gt2::DiscImage bad(path.string()); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "truncated disc must be rejected");
        Check(std::filesystem::remove(path), "failed disc constructor must release the file");
        std::cout << checks << " regression checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
