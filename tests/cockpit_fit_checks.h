#pragma once
#include "gt2view/cockpit_eye_fit.h"
#include <cmath>

template<class Check>
void CockpitFitChecks(Check check) {
    using gt2::CarMeshVertex;
    using namespace gt2view;
    CockpitFit fit;
    fit.valid = true; fit.windshieldZ = -1.0f; fit.dashboardY = 0.7f;
    std::array<CarMeshVertex, 3> triangle{};
    triangle[0].pos[0] = -1; triangle[0].pos[1] = 0.3f; triangle[0].pos[2] = -2;
    triangle[1].pos[0] = 1; triangle[1].pos[1] = 0.3f; triangle[1].pos[2] = 0;
    triangle[2].pos[1] = 0.6f; triangle[2].pos[2] = -2;
    for (auto& vertex : triangle) {
        vertex.texel[0] = (vertex.pos[2] + 2.0f) * 10.0f;
        vertex.color[0] = vertex.texel[0] / 20.0f;
        vertex.palette = 11; vertex.textured = true; vertex.rawTexture = true;
    }
    const auto clipped = ClipCockpitHood(triangle, fit);
    check(clipped.size() == 6, "bonnet clipping retains both triangles of the cut polygon");
    size_t boundary = 0;
    for (const auto& vertex : clipped) {
        check(vertex.pos[2] <= fit.windshieldZ && vertex.pos[1] <= fit.dashboardY, "bonnet stays outside the cabin opening");
        check(vertex.palette == 11 && vertex.textured && vertex.rawTexture, "bonnet keeps source paint and texture flags");
        if (std::abs(vertex.pos[2] - fit.windshieldZ) < 1e-6f) {
            ++boundary;
            check(std::abs(vertex.texel[0] - 10.0f) < 1e-5f && std::abs(vertex.color[0] - 0.5f) < 1e-5f,
                  "cut edges interpolate texture coordinates and colour");
        }
    }
    check(boundary >= 2, "bonnet has interpolated boundary vertices");
    for (auto& vertex : triangle) vertex.pos[1] = 1.0f;
    check(ClipCockpitHood(triangle, fit).empty(), "opaque windscreen polygons above the cowl are excluded");
    for (auto& vertex : triangle) vertex.pos[1] = 0.3f;
    triangle[2] = triangle[0];
    check(ClipCockpitHood(triangle, fit).empty(), "zero-area bonnet triangles are excluded");
    check(!FitCockpit(gt2::CarModel{}).valid, "missing disc geometry has no fabricated bonnet");
    const std::array<std::array<float,2>,5> slopingHood{{{-1.6f,.4f},{-1.3f,.49f},{-1.f,.58f},{-.4f,1.02f},{0.f,1.08f}}};
    check(cockpit_detail::WindshieldSlopeKnee(slopingHood,0,3)==2,
          "a long sloping bonnet ends at the steeper windscreen instead of becoming a raised dashboard deck");
    const std::array<std::array<float,2>,4> curvedGlass{{{-.9f,.6f},{-.8f,.63f},{-.3f,1.04f},{0.f,1.1f}}};
    check(cockpit_detail::WindshieldSlopeKnee(curvedGlass,0,2)==0,
          "a short curved windscreen edge is not mistaken for a bonnet");
    const std::array<std::array<float,2>,4> steepBonnet{{{-2.f,.3f},{-1.35f,.599f},{-.8f,1.025f},{0.f,1.08f}}};
    check(cockpit_detail::WindshieldSlopeKnee(steepBonnet,0,2)==1,
          "a sports prototype's steep bonnet still ends before the more upright windscreen");

    gt2::CarModel car;
    car.lods.resize(1);
    auto& lod = car.lods[0]; lod.scale = 16;
    auto vertex = [&](float x, float y, float z) {
        lod.vertices.push_back({int16_t(std::lround(x * 4096)), int16_t(std::lround(y * 4096)), int16_t(std::lround(z * 4096)), 0});
    };
    for (const auto& yz : std::array<std::array<float, 2>, 5>{{{0.6f, -2.0f}, {0.6f, -0.6f}, {1.1f, 0.0f}, {1.1f, 1.0f}, {0.6f, 2.0f}}}) {
        vertex(-0.8f, yz[0], yz[1]); vertex(0.8f, yz[0], yz[1]);
    }
    for (uint8_t row = 0; row < 4; ++row) {
        gt2::CarPolygon polygon; polygon.primCode = 0x28;
        polygon.vertex = {uint8_t(row * 2), uint8_t(row * 2 + 1), uint8_t(row * 2 + 3), uint8_t(row * 2 + 2)};
        lod.polygons.push_back(polygon);
    }
    vertex(-0.8f, 0, -2); vertex(0.8f, 0, -2); vertex(0.8f, 0, 2); vertex(-0.8f, 0, 2);
    gt2::CarPolygon floor; floor.primCode = 0x28; floor.vertex = {10, 11, 12, 13}; lod.polygons.push_back(floor);
    car.wheels[0].x = car.wheels[1].x = -4096;
    car.wheels[2].x = car.wheels[3].x = 4096;
    const auto fitted = FitCockpit(car);
    check(fitted.valid && std::abs(fitted.windshieldZ + 0.6f) < 0.05f, "cabin finds the bonnet-to-windscreen rise");
    check(fitted.eye[0] < 0 && fitted.eye[2] > fitted.windshieldZ && fitted.eye[1] > fitted.dashboardY + 0.29f,
          "driver sits inside the left seat with clear forward sight");
    check(fitted.eye[1] < fitted.roofY && fitted.roofY < 1.2f, "driver and cabin roof follow the car's scale");
    check(std::abs(fitted.roofFrontZ) < 0.01f && std::abs(fitted.roofRearZ - 1.0f) < 0.01f &&
          std::abs(fitted.rearWindowZ - 2.0f) < 0.01f, "front and rear glazing follow authored profile breaks");
    check(!fitted.openTop && std::abs(fitted.frontCowl[0][0] + 0.8f) < 0.01f &&
          std::abs(fitted.frontHeader[1][0] - 0.8f) < 0.01f, "window corners retain the original shell dimensions");
    check(std::all_of(fitted.cowl.begin(),fitted.cowl.end(),[](const auto& p) {
        return std::abs(p[1]-.6f)<.002f && std::abs(p[2]+.6f)<.002f;
    }), "cowl sections meet the authored hood boundary across the whole width");
    auto mirrorFit=fitted;
    FitCockpitMirror(mirrorFit,car);
    const float frameTop=mirrorFit.mirror[1]+.035f;
    const float glassZ=-.6f+(frameTop-.6f)*1.2f;
    check(mirrorFit.mirror[2]-.018f>glassZ && mirrorFit.mirror[2]<mirrorFit.eye[2],
          "the mirror back clears the windscreen at the top of the frame and stays ahead of the driver");

    auto lowCar = car;
    for (auto& v : lowCar.lods[0].vertices)
        if (v.y > 3000) v.y = int16_t(v.y - 614);
    const auto low = FitCockpit(lowCar);
    check(low.valid && low.roofY < 0.96f && low.eye[1] <= low.roofY - 0.109f,
          "a low sports roof stays at its original height with head clearance");

    auto withSpoiler = car;
    auto& accessory = withSpoiler.lods[0];
    const uint8_t first = uint8_t(accessory.vertices.size());
    accessory.vertices.push_back({-2048, 8192, 8602, 0});
    accessory.vertices.push_back({2048, 8192, 8602, 0});
    accessory.vertices.push_back({0, 8192, 9011, 0});
    gt2::CarPolygon spoiler; spoiler.primCode = 0x20; spoiler.vertex = {first, uint8_t(first + 1), uint8_t(first + 2), 0};
    accessory.polygons.push_back(spoiler);
    const auto belowSpoiler = FitCockpit(withSpoiler);
    check(belowSpoiler.valid && std::abs(belowSpoiler.roofY - fitted.roofY) < 0.001f &&
          std::abs(belowSpoiler.eye[1] - fitted.eye[1]) < 0.001f, "rear accessories do not raise the roof or driver's eye");

    auto openCar = car;
    openCar.lods[0].polygons.erase(openCar.lods[0].polygons.begin() + 2);
    const auto open = FitCockpit(openCar);
    check(open.openTop, "missing overhead shell does not invent a closed roof");

    auto withOverlappingWing = car;
    auto& wingLod = withOverlappingWing.lods[0];
    const uint8_t wingFirst = uint8_t(wingLod.vertices.size());
    for (const auto& point : std::array<std::array<float, 3>, 4>{{{-.7f, 1.25f, .85f}, {.7f, 1.25f, .85f},
                                                                  {.7f, 1.30f, 1.15f}, {-.7f, 1.30f, 1.15f}}})
        wingLod.vertices.push_back({int16_t(std::lround(point[0] * 4096)), int16_t(std::lround(point[1] * 4096)),
                                   int16_t(std::lround(point[2] * 4096)), 0});
    gt2::CarPolygon wing; wing.primCode = 0x28;
    wing.vertex = {wingFirst, uint8_t(wingFirst + 1), uint8_t(wingFirst + 2), uint8_t(wingFirst + 3)};
    wingLod.polygons.push_back(wing);
    const auto underWing = FitCockpit(withOverlappingWing);
    check(underWing.valid && std::abs(underWing.roofY - fitted.roofY) < .001f &&
          std::abs(underWing.roofRearZ - fitted.roofRearZ) < .001f && underWing.eye == fitted.eye,
          "a separate wing over the rear roof does not extend or raise the cabin");

    std::vector<CarMeshVertex> curvedScreen;
    std::vector<gt2::CarPolygon> screenFaces;
    auto screenQuad = [&](const std::array<std::array<float, 3>, 4>& points) {
        gt2::CarPolygon face; face.primCode = 0x28; screenFaces.push_back(face);
        for (size_t k : {0u, 1u, 3u, 1u, 2u, 3u}) {
            CarMeshVertex point{};
            std::copy(points[k].begin(), points[k].end(), point.pos);
            curvedScreen.push_back(point);
        }
    };
    for (float side : {-1.0f, 1.0f}) {
        screenQuad({{{0, .93f, -.45f}, {0, .60f, -1.5f}, {side * .35f, .60f, -1.5f}, {side * .61f, .90f, -.45f}}});
        screenQuad({{{side * .61f, .90f, -.45f}, {side * .35f, .60f, -1.5f},
                     {side * .75f, .66f, -1.2f}, {side * .80f, .75f, -.85f}}});
    }
    std::array<std::array<float, 3>, 2> lowerCorners{}, upperCorners{};
    cockpit_detail::WindowCorners(curvedScreen, screenFaces, -1.5f, .60f, -.45f, .93f, .85f, false,
                                  lowerCorners, upperCorners);
    check(std::abs(lowerCorners[0][0] + .75f) < .001f && std::abs(lowerCorners[1][0] - .75f) < .001f,
          "both triangles of a curved window retain its lower outer corners");
    check(std::abs(upperCorners[0][0] + .61f) < .001f && std::abs(upperCorners[1][0] - .61f) < .001f,
          "completing the cowl width does not move the window header");
    auto offCentreScreen=curvedScreen;
    for (auto& p:offCentreScreen) if (std::abs(p.pos[0])<1e-6f) p.pos[0]=-.0005f;
    cockpit_detail::WindowCorners(offCentreScreen,screenFaces,-1.5f,.60f,-.45f,.93f,.85f,false,lowerCorners,upperCorners);
    check(lowerCorners[0][0]<-.6f && lowerCorners[1][0]>.6f,
          "a quantized centre seam cannot replace an outer windscreen corner on either side");

    auto sillFit=fitted;
    for (size_t side=0;side<2;++side) {
        const float sign=side?1.f:-1.f;
        sillFit.frontCowl[side]={sign*.70f,.65f,-.5f};
        sillFit.rearCowl[side]={sign*.66f,1.10f,1.3f};
    }
    std::vector<CarMeshVertex> sideGlass;
    for (float side:{-1.f,1.f}) {
        const std::array<std::array<float,3>,4> points{{{side*.73f,.68f,-.45f},{side*.73f,.64f,1.2f},
            {side*.68f,1.15f,1.1f},{side*.68f,1.15f,-.35f}}};
        for (size_t i:{0u,1u,2u,0u,2u,3u}) {
            CarMeshVertex p{}; std::copy(points[i].begin(),points[i].end(),p.pos); sideGlass.push_back(p);
        }
    }
    const auto originalCowl=sillFit.frontCowl;
    FitCockpitSideSills(sillFit,sideGlass);
    check(sillFit.sideSillValid && CockpitSideSillAt(sillFit,0,.8f)[1]<.66f &&
          CockpitSideSillAt(sillFit,1,.8f)[1]<.66f,
          "a high rear windscreen cannot lift either door card through its side window");
    bool lowerEnvelope=true;
    for (size_t at=0;at<sideGlass.size();at+=3) {
        const auto tri=std::span(sideGlass).subspan(at,3);
        for (int i=0;i<=20;++i) for (int j=0;j<=20-i;++j) {
            std::array<float,3> p{};
            const float weights[]={float(i)/20,float(j)/20,float(20-i-j)/20};
            for (size_t k=0;k<3;++k) for (size_t axis=0;axis<3;++axis) p[axis]+=tri[k].pos[axis]*weights[k];
            lowerEnvelope &= CockpitSideSillAt(sillFit,size_t(p[0]>=0),p[2])[1]<=p[1]+1e-5f;
        }
    }
    check(lowerEnvelope,"sampled side sills stay below the complete opening, including between profile stations");
    check(sillFit.frontCowl==originalCowl && std::abs(CockpitSideSillAt(sillFit,1,.5f)[0]-.73f)<.001f,
          "side-window fitting reaches the authored door width without moving the windscreen cowl");
    std::array<CarMeshVertex,3> frontalGlass{};
    for (size_t i=0;i<3;++i) {
        frontalGlass[i]=sideGlass[i]; std::swap(frontalGlass[i].pos[0],frontalGlass[i].pos[2]);
    }
    check(!CockpitSideOpening(frontalGlass,sillFit),"windscreen glass does not lower the side sill");

    gt2::CarTexture glazing;
    glazing.paints.resize(1); glazing.indices.assign(256 * 224, 1);
    glazing.paints[0].cluts[0][1] = 2 | (3 << 5) | (3 << 10);
    glazing.paints[0].cluts[0][2] = 5;
    for (size_t y = 60; y < 224; ++y) for (size_t x = 0; x < 256; ++x) glazing.indices[y * 256 + x] = 2;
    std::array<uint16_t, 16> glassMask{}; glassMask[0] = 1u << 1;
    std::vector<CarMeshVertex> shell;
    auto appendQuad = [&](std::array<CarMeshVertex, 4> q) {
        for (size_t i : {0u, 1u, 2u, 0u, 2u, 3u}) shell.push_back(q[i]);
    };
    auto corner = [](float x, float y, float z, float u, float v, bool glass) {
        CarMeshVertex point{};
        point.pos[0] = x; point.pos[1] = y; point.pos[2] = z;
        point.texel[0] = u; point.texel[1] = v;
        point.textured = glass; point.palette = uint8_t(kCockpitGlazingPalette);
        return point;
    };
    appendQuad({corner(-.6f, .6f, -.2f, .5f, .5f, true), corner(.6f, .6f, -.2f, 127.5f, .5f, true),
                corner(.6f, 1.1f, -.2f, 127.5f, 100.5f, true), corner(-.6f, 1.1f, -.2f, .5f, 100.5f, true)});
    appendQuad({corner(-.7f, 1.2f, 0, 0, 0, false), corner(.7f, 1.2f, 0, 0, 0, false),
                corner(.7f, 1.2f, 1, 0, 0, false), corner(-.7f, 1.2f, 1, 0, 0, false)});
    CockpitFit seat; seat.valid = true; seat.windshieldY = .6f; seat.roofFrontY = seat.roofY = 1.2f;
    seat.eye = {-.23f, 1.0f, .2f};
    check(RefineCockpitEye(seat, shell, glazing, glassMask) && seat.eye[1] < .85f && seat.eye[1] > .70f,
          "seat lowers below an opaque screen banner without deleting its pixels");
    check(std::abs(seat.eye[0] + .23f) < 1e-6f && seat.eye[2] == .2f,
          "an unobstructed left seat keeps its lateral and longitudinal position");
    for (size_t i = 0; i < 6; ++i) shell[i].textured = false;
    const auto before = seat.eye;
    check(!RefineCockpitEye(seat, shell, glazing, glassMask) && seat.eye == before,
          "an opaque body cannot be passed by moving the eye above the roof or below the cowl");
}
