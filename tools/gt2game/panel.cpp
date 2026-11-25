#include "game/arcade/arcade_menus.h"
#include "game/shell/title_draw.h"
// The race flow's 2D layers (see panel.h).
#include "panel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "arcade_post_race.h"
#include "game/career/championship.h"
#include "gt2export/png_writer.h"
#include "gt2formats/car_info.h"
#include "gt2formats/course_map.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2game {

namespace {
// The arcade disc (gt2formats/exe_profile.h ExeProfile::arcade): its flow has no GT-mode menus (member 4's fonts are not
// read) and the race overlay's menu assets come in the Simulation layout (arcade_post_race.h LoadArcadeRaceMenuAssets).
bool ArcadeDisc(const gt2::DiscImage& disc) { (void)disc; return gt2::ArcadeFlow(); }
gt2::RaceMenuAssets MenuAssetsOf(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, gt2::RaceMenuAssets::Pictures pictures) {
    return ArcadeDisc(disc) ? LoadArcadeRaceMenuAssets(disc, vol) : gt2::RaceMenuAssets::Load(disc, vol, pictures);
}
} // namespace

Panels::Panels(gt2view::SceneRenderer& renderer, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol)
    : view_(renderer), arcade_(ArcadeDisc(disc)), fonts_(arcade_ ? gt2::MenuFonts{} : gt2::LoadMenuFonts(gt2::LoadOverlayImage(disc, 4))),
      overlayAssets_(gt2::raceui::RaceOverlayAssets::Load(disc, vol)), licenceAssets_(MenuAssetsOf(disc, vol, gt2::RaceMenuAssets::Pictures::kLicence)),
      settingsAssets_(arcade_ ? licenceAssets_ : MenuAssetsOf(disc, vol, gt2::RaceMenuAssets::Pictures::kSettings)), renderer_(renderer), vol_(vol), disc_(disc) {
    // One console image: the race overlay's areas (x 384..575) and the menus' font page (tpage 10 = x 640..703).
    vram_ = overlayAssets_.vram;
    if (arcade_) {
        const auto assets = gt2::arcade::ArcadeMenuAssets::Load(disc, vol);
        arcadeFont_ = assets.fonts[2];
        for (int y = 256; y < 512; ++y)
            for (int x = 896; x < 960; ++x) vram_[size_t(y)*1024+x] = assets.vram.Word(x,y);
        Upload();
        return;
    }
    gt2::MenuVram font;
    font.UploadTimToPage(vol.Read("arcade/gtmode_font.tim"), 10); // gt_menu_images.h VRAM layout
    for (int y = 0; y < 256; y++)
        for (int x = 640; x < 704; x++) vram_[size_t(y) * 1024 + size_t(x)] = font.Word(x, y);
    Upload();
}

void Panels::FullScreenModel(std::vector<gt2::MenuPrim> prims, Screen which, size_t modelAt, const std::optional<gt2::screens::PostRaceModel>& model, uint32_t carId,
                             int paint) {
    screen_ = std::move(prims), screenKind_ = which;
    model_ = model;
    modelAt_ = modelAt;
    modelCar_ = carId;
    modelPaint_ = paint;
}

void Panels::Overlay(const std::vector<gt2::raceui::Gp0Prim>& prims) {
    overlay_.insert(overlay_.end(), prims.begin(), prims.end());
    static std::FILE* const log = [] { // dev aid (panel.h)
        const char* path = std::getenv("GT2_RACE_OVERLAY_LOG");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    if (!log || prims.empty()) return;
    static int frame = 0;
    std::fprintf(log, "# frame %d\n", frame++);
    for (const std::string& line : gt2::raceui::ListGp0(prims)) std::fprintf(log, "%s\n", line.c_str());
    std::fflush(log);
}

void Panels::SetSound(bool on) {
    soundOn_ = on;
    if (!on) sfx_.reset();
}

void Panels::Sound(int id) {
    if (!soundOn_ || soundFailed_ || id < 0) return;
    if (!sfx_) {
        try {
            sfx_ = std::make_unique<gt2::audio::MenuAudio>();
            if (arcade_) sfx_->LoadArcade(vol_, gt2::LoadExeImage(disc_)); // the arcade EXE's effect table (arcade_disc.md 17.5)
            else sfx_->Load(vol_, gt2::LoadExeImage(disc_), gt2::LoadOverlayImage(disc_, 4)); // the EXE's effect table, sound/sys.ins
            std::string error;
            if (!sfx_->OpenDevice(error)) {
                std::printf("race screens: no sound device (%s)\n", error.c_str());
                sfx_.reset();
                soundFailed_ = true;
                return;
            }
        } catch (const std::exception& e) {
            std::printf("race screens: no sound (%s)\n", e.what());
            sfx_.reset();
            soundFailed_ = true;
            return;
        }
    }
    sfx_->Sound(id);
}

void Panels::SoundFrame() {
    if (sfx_) sfx_->Frame();
}

void Panels::CloseSound() { sfx_.reset(); }

void Panels::Upload() {
    view_.UploadVram(vram_);
    uploaded_ = 0;
}

void Panels::Box(int x, int y, int w, int h, uint32_t rgb, bool semi) {
    gt2::MenuPrim p;
    p.kind = gt2::MenuPrim::kTile;
    p.x[0] = int16_t(x);
    p.y[0] = int16_t(y);
    p.w = int16_t(w);
    p.h = int16_t(h);
    p.colour[0] = rgb & 0xFFFFFF;
    p.semi = semi;
    p.tpage = 0; // mode 0: B/2 + F/2
    prims_.push_back(p);
}

namespace {
// The menu font has no glyphs for a few ASCII characters (they come out as a box): our texts avoid them.
std::u16string Printable(const std::string& text) {
    std::string t = text;
    for (char& c : t)
        if (c == '_' || c == ':' || c == '>' || c == '<' || c == '[' || c == ']' || c == '=') c = ' ';
    return gt2::MenuTextWriter::Widen(t);
}
} // namespace

// ':' has no glyph either: drawn as two dots (times "1:53.004").
constexpr int kColonWidth = 6;

int Panels::Text(int x, int y, const std::string& text, uint32_t colour) {
    if (arcade_) {
        gt2::MenuOtSlot ot;
        const int width = gt2::shell::AddText(ot, arcadeFont_, text, x, y + 18, 1, colour, 0);
        ot.Emit(prims_);
        return width;
    }
    gt2::MenuTextWriter w(fonts_, prims_);
    int width = 0;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t colon = text.find(':', at);
        const std::string part = text.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        width += w.Draw(Printable(part), true, false, x + width, y, colour);
        if (colon == std::string::npos) break;
        const uint32_t dot = colour == 0x808080 ? 0xC0C0C0 : (colour & 0xFFFFFF);
        Box(x + width + 2, y + 12, 2, 2, dot);
        Box(x + width + 2, y + 19, 2, 2, dot);
        width += kColonWidth;
        at = colon + 1;
    }
    return width;
}

void Panels::EmoteIcon(int x,int y,int scale,uint8_t emote) {
    if(emote<1||emote>6)return;
    auto rgb=[](int red,int green,int blue){return uint32_t(red|(green<<8)|(blue<<16));};
    auto ellipse=[&](float cx,float cy,float rx,float ry,uint32_t color) {
        const int top=int(std::floor((cy-ry)*scale)),bottom=int(std::ceil((cy+ry)*scale));
        for(int row=top;row<bottom;++row){const float dy=((float(row)+.5f)/scale-cy)/ry;
            if(std::abs(dy)>=1)continue;const float half=rx*std::sqrt(1-dy*dy)*scale;
            const int left=int(std::ceil(cx*scale-half)),right=int(std::floor(cx*scale+half));
            if(right>=left)Box(x+left,y+row,right-left+1,1,color);
        }
    };
    const uint32_t ink=rgb(74,42,20);
    if(emote==6) {
        ellipse(5.1f,5.5f,3.8f,3.8f,rgb(246,63,103));ellipse(10.9f,5.5f,3.8f,3.8f,rgb(246,63,103));
        for(int row=6*scale;row<15*scale;++row){int half=int((15*scale-row)*.76f);Box(x+8*scale-half,y+row,half*2+1,1,rgb(246,63,103));}
        ellipse(4.4f,4.3f,.8f,1.3f,rgb(255,164,181));return;
    }
    ellipse(8,8.5f,7.8f,7.5f,rgb(141,90,21));
    for(int row=0;row<15*scale;++row){float dy=(float(row)+.5f)/scale-7.5f;
        const float half=std::sqrt(std::max(0.f,56.25f-dy*dy))*scale;
        const int left=int(std::ceil(8*scale-half)),right=int(std::floor(8*scale+half));
        const float t=float(row)/(15*scale);
        Box(x+left,y+row,right-left+1,1,rgb(255,int(222-48*t),int(81-44*t)));
    }
    ellipse(5,3.2f,2.5f,.7f,rgb(255,235,141));
    ellipse(4.8f,6,1,1.4f,ink);ellipse(11.2f,6,1,1.4f,ink);
    ellipse(4.55f,5.6f,.3f,.35f,rgb(255,255,244));ellipse(10.95f,5.6f,.3f,.35f,rgb(255,255,244));
    if(emote==1||emote==4||emote==5) {
        for(int i=0;i<=16;++i){float px=4+float(i)*.5f;float curve=1-(px-8)*(px-8)/16;
            ellipse(px,(emote==1?10+2*curve:12-1.5f*curve),.4f,.4f,ink);}
    }
    if(emote==2){ellipse(8,10.8f,3.6f,2.7f,ink);ellipse(8,9.5f,2.6f,.7f,rgb(255,253,239));ellipse(8,12,1.7f,.7f,rgb(236,111,111));}
    if(emote==3)ellipse(8,11,1.6f,2.1f,ink);
    if(emote==4){ellipse(3.3f,9.1f,.75f,2.4f,rgb(70,171,242));ellipse(12.7f,9.1f,.75f,2.4f,rgb(70,171,242));ellipse(8,12,1.5f,.8f,ink);}
    if(emote==5)for(int i=0;i<8;++i){float t=float(i)/8;ellipse(3.4f+3*t,3.7f+1.2f*t,.45f,.35f,ink);ellipse(12.6f-3*t,3.7f+1.2f*t,.45f,.35f,ink);}
}
void Panels::CoursePreview(const std::string& course,int x,int y,int w,int h) {
    if(previewCourse_!=course){
        previewCourse_=course;previewValid_=false;
        try {
            const auto pic=gt2::LoadCoursePicture(vol_,course);
            if(pic.words!=47||pic.rows!=200||pic.clut.size()!=16)throw std::runtime_error("invalid course preview dimensions");
            std::copy(pic.clut.begin(),pic.clut.end(),vram_.begin()+256*1024);
            for(size_t row=0;row<pic.rows;++row)std::copy_n(pic.image.begin()+row*pic.words,pic.words,vram_.begin()+(257+row)*1024);
            Upload();previewValid_=true;
        }catch(const std::exception& ex){std::fprintf(stderr,"course preview %s: %s\n",course.c_str(),ex.what());}
    }
    if(!previewValid_){TextCentre(x+w/2,y+h/2,"No preview");return;}
    gt2::MenuPrim p;p.kind=gt2::MenuPrim::kPolyFT4;p.tpage=0x10;p.clut=0x4000;
    for(int i=0;i<4;++i){p.x[i]=int16_t(x+(i&1?w:0));p.y[i]=int16_t(y+(i&2?h:0));p.tu[i]=uint8_t(i&1?188:0);p.tv[i]=uint8_t(i&2?201:1);}
    prims_.push_back(p);
}
void Panels::EmoteWheel(int selected) {
    emoteWheelFrame_=true;
    // Seven rounded annular sectors, including an empty cancel choice, with empty space in the centre.
    for(int sector=0;sector<7;++sector)for(int segment=0;segment<12;++segment) {
        const float start=float(sector)*.8975979f-.425f;
        const float angle[4]={start+float(segment)*.85f/12,start+float(segment+1)*.85f/12,start+float(segment)*.85f/12,start+float(segment+1)*.85f/12};
        gt2::MenuPrim p;p.kind=gt2::MenuPrim::kPolyF4;p.semi=true;
        p.colour[0]=sector==selected?0x9c7037:0x30251b;
        for(int k=0;k<4;++k){float radius=k<2?80.f:140.f;p.x[k]=int16_t(std::lround(256+std::sin(angle[k])*radius));p.y[k]=int16_t(std::lround(230-std::cos(angle[k])*radius));}
        prims_.push_back(p);
    }
    static constexpr const char* names[]={"Cancel","Joy","Laugh","Wow","Cry","Angry","Love"};
    for(int i=1;i<7;++i){const float angle=float(i)*.8975979f;EmoteIcon(int(std::lround(256+110*std::sin(angle)))-24,int(std::lround(230-110*std::cos(angle)))-24,3,uint8_t(i));}
    TextCentre(256,212,names[std::clamp(selected,0,6)]);
    TextCentre(256,236,"L3 / A / Cross / Enter: confirm");
    TextCentre(256,385,"Left stick: choose. Empty: cancel.");
}

void Panels::WorldLabel(const std::string& text,size_t slot,const float* matrix,std::vector<gt2view::DrawItem>& items,uint8_t emote) {
    if(slot>=6)return;
    prims_.clear();const auto label=text.substr(0,23);TextCentre(256,230,label);
    if(emote)EmoteIcon(264+Width(label)/2,226,2,emote);
    const auto first=items.size();
    view_.BuildRange(prims_,512,480,4.0f/3.0f,gt2view::SceneRendererState::kNameTagVertexBase+uint32_t(slot)*1536,1536,items);
    for(size_t i=first;i<items.size();++i){std::copy(matrix,matrix+16,items[i].mvp);items[i].space=gt2view::kSpaceWorld;}
    prims_.clear();
}

int Panels::Width(const std::string& text) const {
    if (arcade_) return arcadeFont_.TextWidth(text, 1);
    std::vector<gt2::MenuPrim> none;
    gt2::MenuTextWriter w(fonts_, none);
    int width = 0;
    for (char c : text) width += c == ':' ? kColonWidth : 0;
    std::string rest = text;
    rest.erase(std::remove(rest.begin(), rest.end(), ':'), rest.end());
    return width + w.Width(Printable(rest), true, false);
}

int Panels::TextRight(int right, int y, const std::string& text, uint32_t colour) { return Text(right - Width(text), y, text, colour); }
int Panels::TextCentre(int centre, int y, const std::string& text, uint32_t colour) { return Text(centre - Width(text) / 2, y, text, colour); }

void Panels::DrawMenu(const Menu& m, int top) {
    // The arcade flow has no such panels: its race starts at once and the race-end display's X goes on to the replay
    // (arcade_disc.md 17.1); the fields in which the flow passes ours (arcade_mode.cpp) show the race alone.
    const int lines = int(m.lines.size() + m.items.size() + m.footer.size()) + (m.title.empty() ? 0 : 2) + (m.items.empty() ? 0 : 1) + (m.footer.empty() ? 0 : 1);
    const int height = lines * kLine + 24;
    const int left = (gt2::MenuCanvas::kWidth - m.width) / 2;
    Box(left, top, m.width, height, 0x101820, true);
    Box(left, top, m.width, 2, 0xC04040);
    Box(left, top + height - 2, m.width, 2, 0xC04040);
    int y = top + 12;
    if (!m.title.empty()) {
        TextCentre(gt2::MenuCanvas::kWidth / 2, y, m.title, 0x80A0FF);
        y += 2 * kLine;
    }
    for (const std::string& line : m.lines) {
        Text(left + 16, y, line);
        y += kLine;
    }
    if (!m.items.empty()) y += kLine;
    for (size_t i = 0; i < m.items.size(); i++) {
        const bool sel = int(i) == m.selected;
        if (sel) Box(left + 8, y + 7, m.width - 16, kLine, 0x604020, true);
        Text(left + 24, y, m.items[i], sel ? 0x80FFFF : 0x808080);
        y += kLine;
    }
    if (!m.footer.empty()) y += kLine;
    for (const std::string& line : m.footer) {
        Text(left + 16, y, line, 0x606060);
        y += kLine;
    }
}

void Panels::Append(float windowAspect, std::vector<gt2view::DrawItem>& items) {
    // The race overlay's menus and its race screens need different images at the same VRAM places (arc_font.tim vs
    // font/racefont.dat at (384, 0), as in the console while each is shown): the rows are re-uploaded on a change.
    const int want = !screen_.empty() ? (screenKind_ == Screen::kLicence ? 1 : 2) : 0;
    if (want != uploaded_) {
        if (want == 0) view_.UploadVram(vram_);
        else view_.UploadVram(MenuAssets(screenKind_).vram);
        uploaded_ = want;
    }
    if (!screen_.empty() && model_) {
        // The frame through a MenuView on the panel rows: the clear and the floor disc at the far depth, the model
        // (depth-tested, clipped to its environment), then the views in front (race_result_screens.h PostRaceModel).
        if (!modelFrame_) {
            modelFrame_ = std::make_unique<gt2view::MenuView>(renderer_, gt2view::PanelView::kRowBase);
            modelFrame_->SetFrameSize(gt2::RaceMenuAssets::kScreenWidth, gt2::RaceMenuAssets::kScreenHeight);
            modelFrame_->SetInterpolatedPolygons(true); // as PanelView draws them (the dithered headers' pixel runs do not fit)
        }
        if (!modelView_) {
            modelView_ = std::make_unique<gt2view::MenuCarView>(renderer_, vol_, carAssets_);
            modelView_->SetFrameSize(gt2::RaceMenuAssets::kScreenWidth, gt2::RaceMenuAssets::kScreenHeight);
        }
        const bool loaded = modelView_->Use(model_->trophy ? gt2::PackCarId("gtprz") : modelCar_, model_->carWheels);
        // The reflection pass: the car's samples the course's map left in VRAM by the race (page 9, CLUT 0x7FD7, model
        // +9 = 0x40); the trophy's map (0x80059800 uploads it with the model) is not loaded: no pass. The licence BONUS's
        // plate is drawn by 0x80048528 (no car draw): no reflection, shadow or generated wheels.
        modelView_->SetReflection(model_->envPage, model_->envClut, model_->trophy || !model_->carWheels ? uint8_t(0) : model_->reflection);
        modelView_->SetShadow(!model_->trophy && model_->carWheels); // 0x80048528 (the trophy / the plate) draws no ground shadow
        gt2::MenuFrame frame;
        frame.prims = screen_;
        frame.layer3dAt = modelAt_;
        const gt2::menu::MenuCarProjection projection = model_->Projection(true);
        const int paint = model_->trophy ? 0 : modelPaint_;
        modelFrame_->Build(frame, windowAspect, items, false, [&](std::vector<gt2view::DrawItem>& layer) {
            if (loaded) modelView_->Append(layer, projection, paint, windowAspect, false);
        });
        return;
    }
    if (!screen_.empty()) {
        view_.Build(screen_, gt2::RaceMenuAssets::kScreenWidth, gt2::RaceMenuAssets::kScreenHeight, windowAspect, items);
        return;
    }
    view_.BuildOverlay(overlay_, windowAspect, items);
    if(emoteWheelFrame_)view_.BuildRange(prims_,512,480,windowAspect,gt2view::SceneRendererState::kEmoteWheelVertexBase,16384,items);
    else view_.Build(prims_, windowAspect, items);
}

// ---------------------------------------------------------------- --race-screen-check

namespace {

std::vector<uint8_t> Rgba(const std::vector<uint16_t>& vram, int x0, int y0, int w, int h) {
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint16_t c = vram[size_t((y0 + y) & 511) * 1024 + size_t((x0 + x) & 1023)];
            uint8_t* p = &out[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = uint8_t((c & 31) << 3), p[1] = uint8_t(((c >> 5) & 31) << 3), p[2] = uint8_t(((c >> 10) & 31) << 3), p[3] = 255;
        }
    return out;
}

// The race-end display's inputs as the original holds them (a 2 MB RAM dump written by gt2play --prims (.ram.bin) or
// gt2run session snap=): 0x8002B170 reads the timers 0x800AF226 / 0x800AF228, the sub-mode 0x801D5866, the series
// block 0x801D5DF4 (+2 race count, +0x88 total points, +0x8E race points), the lap count 0x801D586B, the car count
// u8 0x801D58B6 and per car (0xB40 bytes from 0x800A9C80): position +0x184, laps s16 +0x3C, finished +0x130, total time
// +0x1B4; the car names are the race slots' (0x801D5948 + car * 0xD0). The addresses are Simulation ones; `profile` maps them to
// the dump's build (gt2formats/exe_profile.h, the race map: Arcade v1.1 race state -0x310, race block / results -0x5A0;
// identity for a Simulation dump).
gt2::raceui::RaceEndState RaceEndStateFromRam(const std::vector<uint8_t>& ram, const gt2::ExeProfile& profile) {
    auto at = [&](uint32_t sim) -> const uint8_t* {
        const size_t o = profile.Race(sim) & 0x1FFFFF;
        if (o + 4 > ram.size()) throw std::runtime_error("race-end state: read outside the RAM dump");
        return &ram[o];
    };
    auto u8 = [&](uint32_t a) { return *at(a); };
    auto s16 = [&](uint32_t a) { const uint8_t* p = at(a); return int16_t(uint16_t(p[0] | p[1] << 8)); };
    auto s32 = [&](uint32_t a) { const uint8_t* p = at(a); return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24); };
    gt2::raceui::RaceEndState s;
    s.subMode = u8(0x801D5866u);
    s.timer = s16(0x800AF226u);
    s.auxTimer = s16(0x800AF228u);
    s.seriesRaces = s16(0x801D5DF6u);
    s.lapCount = u8(0x801D586Bu);
    const int cars = std::min<int>(u8(0x801D58B6u), 6);
    auto row = [&](int car) {
        gt2::raceui::RaceEndRow r;
        const uint32_t c = 0x800A9C80u + uint32_t(car) * 0xB40;
        for (uint32_t a = 0x801D5948u + uint32_t(car) * 0xD0; u8(a) != 0 && r.name.size() < 0x40; a++) r.name.push_back(char(u8(a)));
        r.player = car == 0;
        r.car = car;
        r.finishTime = s32(c + 0x1B4);
        r.finished = u8(c + 0x130) != 0;
        r.laps = s16(c + 0x3C);
        r.racePoints = int8_t(u8(0x801D5E82u + uint32_t(car)));
        r.totalPoints = int8_t(u8(0x801D5E7Cu + uint32_t(car)));
        return r;
    };
    for (int p = 1; p <= 6; p++)
        for (int car = 0; car < 6; car++)
            if (int8_t(u8(0x800A9C80u + 0x184 + uint32_t(car) * 0xB40)) == p) { s.rows.push_back(row(car)); break; }
    s.rows.resize(std::min(s.rows.size(), size_t(cars)));
    gt2::career::SeriesBlock series{};
    std::memcpy(&series, at(gt2::career::kSeriesAddress), sizeof(series));
    std::array<int8_t, 6> order{};
    gt2::career::ChampionshipOrder(series, order);
    for (int i = 0; i < cars; i++)
        if (order[size_t(i)] >= 0) s.standings.push_back(row(order[size_t(i)]));
    return s;
}

std::vector<uint8_t> ReadBinary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string StripIndex(const std::string& line) { // "123 RECT ..." -> "RECT ..."
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') i++;
    return i > 0 && i < line.size() && line[i] == ' ' ? line.substr(i + 1) : line;
}

} // namespace

int RunRaceScreenCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::vector<std::string>& args) {
    using namespace gt2::raceui;
    if (args.size() < 3) throw std::runtime_error("--race-screen-check <pause | licence-end> <capture.txt> <out.png> [state]");
    const std::string screen = args[0], capturePath = args[1], outPath = args[2];
    auto num = [&](size_t i, long fallback) { return i < args.size() ? std::strtol(args[i].c_str(), nullptr, 0) : fallback; };
    const RaceOverlayAssets assets = RaceOverlayAssets::Load(disc, vol);
    std::vector<Gp0Prim> ours;
    if (screen == "pause") {
        PauseMenu m;
        m.selection = int8_t(num(3, 0));
        m.counter = int8_t(num(4, 20));
        ours = BuildPauseFrame(assets, m);
    } else if (screen == "licence-end") {
        RaceEndState s;
        s.subMode = 3;
        s.timer = int16_t(num(3, 0x92));
        s.licenceResult = int32_t(num(10, 1));
        s.licenceTime = uint32_t(num(4, 0));
        for (size_t k = 0; k < 4; k++) s.medalTimes[k] = uint32_t(num(5 + k, 0));
        s.fourthPrizeCounts = num(9, 0) != 0;
        ours = BuildRaceEndFrame(assets, s);
    } else if (screen == "race-end") { // state from the RAM dump: ram=<file> [timer=N] (default: the dump's timer)
        std::string ramPath;
        int timer = -1000;
        for (size_t i = 3; i < args.size(); i++) {
            if (args[i].rfind("ram=", 0) == 0) ramPath = args[i].substr(4);
            else if (args[i].rfind("timer=", 0) == 0) timer = std::atoi(args[i].c_str() + 6);
        }
        if (ramPath.empty()) ramPath = capturePath + ".ram.bin";
        RaceEndState s = RaceEndStateFromRam(ReadBinary(ramPath), gt2::ProfileOf(disc));
        if (timer != -1000) {
            if (s.auxTimer >= 0) s.auxTimer = int16_t(s.auxTimer + (timer - s.timer));
            s.timer = int16_t(timer);
        }
        std::printf("race-screen-check race-end: sub-mode %d timer %d aux %d series races %d, %zu rows, %zu standings\n", s.subMode, s.timer, s.auxTimer, s.seriesRaces,
                    s.rows.size(), s.standings.size());
        ours = BuildRaceEndFrame(assets, s);
    } else {
        throw std::runtime_error("--race-screen-check: unknown screen " + screen);
    }
    const std::vector<std::string> ourLines = ListGp0(ours);

    // The capture: the lines of its second displayed frame (between the second and third "GP1 05" display switches).
    std::ifstream in(capturePath);
    if (!in) throw std::runtime_error("cannot read " + capturePath);
    std::vector<std::string> frame;
    int switches = 0;
    int originY = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("# ", 0) == 0) continue;
        if (line.rfind("GP1 05", 0) == 0) {
            if (++switches > 2) break;
            continue;
        }
        if (switches != 2 || line.rfind("GP1", 0) == 0) continue;
        const std::string l = StripIndex(line);
        if (l.rfind("E3 ", 0) == 0) originY = int((std::strtoul(l.c_str() + 3, nullptr, 16) >> 10) & 0x1FF);
        frame.push_back(l);
    }
    // Where our list sits in the frame: the longest run of equal lines.
    size_t bestAt = 0, bestLen = 0;
    for (size_t at = 0; at < frame.size(); at++) {
        size_t n = 0;
        while (n < ourLines.size() && at + n < frame.size() && frame[at + n] == ourLines[n]) n++;
        if (n > bestLen) bestLen = n, bestAt = at;
    }
    std::printf("race-screen-check %s: %zu primitives of ours; %zu equal in sequence at capture line %zu of %zu (frame at y %d)\n", screen.c_str(), ourLines.size(), bestLen,
                bestAt, frame.size(), originY);
    if (const char* listPath = std::getenv("GT2_RACE_SCREEN_LIST")) { // dev aid: ours and the capture's lines from the match on
        if (std::FILE* f = std::fopen(listPath, "w")) {
            for (size_t k = 0; k < std::max(ourLines.size(), frame.size() - bestAt); k++)
                std::fprintf(f, "%-100s | %s\n", k < ourLines.size() ? ourLines[k].c_str() : "", bestAt + k < frame.size() ? frame[bestAt + k].c_str() : "");
            std::fclose(f);
        }
    }
    if (bestLen < ourLines.size()) {
        std::printf("  first difference: ours     %s\n", ourLines[bestLen].c_str());
        std::printf("                    captured %s\n", bestAt + bestLen < frame.size() ? frame[bestAt + bestLen].c_str() : "(end of frame)");
    }
    // The capture's own primitives of that range rasterised with the capture's VRAM, ours with our VRAM, both on black.
    const size_t end = std::min(frame.size(), bestAt + ourLines.size());
    const std::vector<std::string> captured(frame.begin() + std::ptrdiff_t(bestAt), frame.begin() + std::ptrdiff_t(end));
    std::vector<uint16_t> captureVram(size_t(1024) * 512, 0);
    {
        std::ifstream raw(capturePath + ".vram.bin", std::ios::binary);
        if (!raw) throw std::runtime_error("cannot read " + capturePath + ".vram.bin");
        raw.read(reinterpret_cast<char*>(captureVram.data()), std::streamsize(captureVram.size() * 2));
    }
    const std::vector<uint8_t> frameRgba = Rgba(captureVram, 0, originY, 320, 240);
    OverlayCanvas theirs(captureVram), mine(assets.vram);
    theirs.Clear(0, 0, 320, 240), mine.Clear(0, 0, 320, 240);
    theirs.Draw(ParseGp0Lines(captured), 0);
    mine.Draw(ours, 0);
    size_t differing = 0;
    std::vector<uint8_t> diff(size_t(320) * 240 * 4, 0);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            const bool d = (theirs.At(x, y) & 0x7FFF) != (mine.At(x, y) & 0x7FFF);
            differing += d ? 1 : 0;
            uint8_t* p = &diff[(size_t(y) * 320 + size_t(x)) * 4];
            p[0] = d ? 255 : 0, p[3] = 255;
        }
    const std::vector<uint8_t> a = Rgba(theirs.Vram(), 0, 0, 320, 240), b = Rgba(mine.Vram(), 0, 0, 320, 240);
    std::vector<uint8_t> side(size_t(1280) * 240 * 4);
    const std::vector<uint8_t>* parts[4] = {&frameRgba, &a, &b, &diff};
    for (size_t k = 0; k < 4; k++)
        for (size_t y = 0; y < 240; y++) std::copy_n(parts[k]->begin() + std::ptrdiff_t(y * 320 * 4), 320 * 4, side.begin() + std::ptrdiff_t((y * 1280 + k * 320) * 4));
    gt2::WritePngRgba(outPath, 1280, 240, side);
    std::printf("race-screen-check %s: %zu differing pixels of 320 x 240 (the original's primitives vs ours, both on black); wrote %s\n", screen.c_str(), differing,
                outPath.c_str());
    return (bestLen == ourLines.size() && differing == 0) ? 0 : 1;
}

} // namespace gt2game
