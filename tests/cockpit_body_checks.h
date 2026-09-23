#pragma once
#include "gt2view/cockpit_eye_fit.h"
#include "gt2vfs/gtfs.h"
#include <fstream>
#include <iostream>

template<class Check>
void CockpitBodyChecks(Check check) {
    gt2::CarTexture texture;
    texture.paints.resize(2);
    texture.indices.assign(256 * 224, 1);
    auto color = [](int r, int g, int b) { return uint16_t(r | (g << 5) | (b << 10)); };
    for (auto& paint : texture.paints) {
        paint.cluts[0][1] = color(2, 3, 3);
        paint.cluts[0][2] = color(2, 2, 2);
        paint.cluts[0][3] = color(2, 3, 3);
        paint.cluts[0][4] = color(2, 4, 9);
        paint.cluts[0][5] = color(1, 2, 1);
        paint.cluts[0][6] = color(3, 3, 2);
    }
    texture.paints[1].cluts[0][3] = color(8, 9, 9);
    const auto masks = gt2view::CockpitGlassMasks(texture);
    check(masks[0] == ((1u << 1) | (1u << 5)), "glazing includes dark green but keeps grey/warm trim, blue decals and paint-dependent colours");
    gt2::CarModel model;
    model.lods.resize(1);
    auto& lod = model.lods[0]; lod.scale = 16;
    lod.vertices = {{-2048, 3072, -2048, 0}, {2048, 3072, -2048, 0},
                    {2048, 4096, 0, 0}, {-2048, 4096, 0, 0},
                    {-2048, 3072, -8192, 0}, {2048, 3072, -8192, 0}, {0, 3584, -4096, 0}};
    gt2::CarPolygon window;
    window.primCode = 0x2d; window.vertex = {0, 1, 2, 3};
    window.u = {0, 8, 8, 0}; window.v = {0, 0, 8, 8};
    lod.polygons.push_back(window);
    auto bonnet = window; bonnet.primCode = 0x25; bonnet.vertex = {4, 5, 6, 0}; lod.polygons.push_back(bonnet);
    gt2view::CockpitFit fit;
    fit.valid = true; fit.windshieldZ = -.5f; fit.windshieldY = .75f;
    fit.roofFrontZ = 0; fit.roofRearZ = 1; fit.roofY = 1.1f;
    fit.rearWindowZ = 1.5f; fit.rearWindowY = .75f;
    const auto body = gt2view::BuildCockpitBody(model, texture, fit);
    check(body.exteriorPolygons == std::vector<size_t>{1}, "cockpit reflections preserve the bonnet and exclude removed glazing");
    check(body.vertices.size() == 3 && body.windowTriangles == 2, "window glass is removed geometrically, never the bonnet");
    gt2::CarMeshOptions options; options.wheels = false;
    const auto original = gt2::BuildCarMesh(model, options);
    if (body.vertices.size() == 3) for (size_t i = 0; i < 3; ++i) for (size_t axis = 0; axis < 3; ++axis)
        check(body.vertices[i].pos[axis] == original[i + 6].pos[axis], "window cutting preserves the original bonnet");
    check(texture.paints[0].cluts[0][1] == color(2, 3, 3) && texture.indices[0] == 1,
          "preparing a cockpit does not edit source car textures");
    auto lining = original.front();
    gt2view::CockpitLining(lining);
    check(lining.textured && lining.rawTexture && lining.palette == gt2view::kCockpitLiningPalette,
          "inner lining retains texture alpha and bypasses exterior vertex colour");
    check(gt2view::CockpitSourcePalette(lining.palette) == original.front().palette &&
          gt2view::CockpitSourcePalette(gt2view::kCockpitGlazingPalette + 15) == 15,
          "visibility maps both cockpit material ranges to original palette indices");
    lod.vertices.insert(lod.vertices.end(), {{-4096, 3482, 0, 0}, {-3891, 3891, 0, 0}, {-4096, 3891, 410, 0}});
    auto mirror = bonnet; mirror.vertex = {7, 8, 9, 0}; lod.polygons.push_back(mirror);
    const auto withMirror = gt2view::BuildCockpitBody(model, texture, fit);
    check(withMirror.vertices.size() == 6 && withMirror.windowTriangles == 2,
          "a teal exterior mirror outside the cabin envelope is not cut as window glass");
    if (withMirror.vertices.size() == 6) {
        check(withMirror.vertices[3].textured && withMirror.vertices[3].palette == 0,
              "exterior mirror retains its original material instead of cabin lining");
    }
    auto shell = fit;
    for (size_t side = 0; side < 2; ++side) {
        const float sign = side ? 1.f : -1.f;
        shell.frontCowl[side] = {sign * .75f, .75f, -.5f}; shell.rearCowl[side] = {sign * .75f, .75f, 1.5f};
        shell.frontHeader[side] = {sign * .65f, 1.15f, 0}; shell.rearHeader[side] = {sign * .65f, 1.15f, 1.f};
    }
    std::array<gt2::CarMeshVertex, 3> mounted{};
    const float positions[3][3] = {{-.715f, .85f, 0}, {-1.f, .95f, 0}, {-.95f, .85f, .1f}};
    for (size_t i = 0; i < 3; ++i) std::copy(positions[i], positions[i] + 3, mounted[i].pos);
    check(gt2view::cockpit_detail::OutsideCabinShell(mounted, shell),
          "an exterior mirror may retain one mounting point just inside the cabin boundary");
    mounted[0].pos[0] = -.65f;
    check(!gt2view::cockpit_detail::OutsideCabinShell(mounted, shell),
          "a body panel reaching substantially inside the cabin is not a mirror");
    mounted[0].pos[0] = -.715f; mounted[1].pos[1] = 1.14f;
    check(!gt2view::cockpit_detail::OutsideCabinShell(mounted, shell),
          "an upper window pillar is not excluded by the lower mirror mounting rule");
    std::array<gt2::CarMeshVertex,3> sidewall{};
    const float wall[3][3]={{-.76f,.74f,.2f},{-.76f,.74f,1.6f},{-.77f,.1f,1.1f}};
    for (size_t i=0;i<3;++i) std::copy(wall[i],wall[i]+3,sidewall[i].pos);
    check(gt2view::cockpit_detail::InnerSidewall(sidewall,shell),
          "lower interior door and rear quarter panels receive the same lining as the window surround");
    for (auto& p:sidewall) p.pos[2]-=2.2f;
    check(!gt2view::cockpit_detail::InnerSidewall(sidewall,shell),"side lining leaves the front fender and bonnet original");
    sidewall[0].pos[2]=-.85f;sidewall[1].pos[2]=-.3f;sidewall[2].pos[2]=-.6f;
    check(gt2view::cockpit_detail::InnerSidewall(sidewall,shell),
          "a lower side decal extending into the cabin receives lining even if its centre lies ahead of the cowl");
    sidewall[1].pos[2]=shell.frontCowl[0][2];
    check(!gt2view::cockpit_detail::InnerSidewall(sidewall,shell),
          "a fender decal ending at the front cowl remains outside the interior lining");
    for (size_t i=0;i<3;++i) std::copy(wall[i],wall[i]+3,sidewall[i].pos);
    for (auto& p:sidewall) p.pos[1]=.9f;
    check(!gt2view::cockpit_detail::InnerSidewall(sidewall,shell),"an external mirror above the door belt keeps its exterior material");
    for (size_t i=0;i<3;++i) std::copy(wall[i],wall[i]+3,sidewall[i].pos);
    sidewall[0].pos[1]=.8f;sidewall[1].pos[1]=1.15f;sidewall[2].pos[1]=.8f;
    check(gt2view::cockpit_detail::ExtendedWindowFace(sidewall,shell,.7f),
          "proven side glazing can extend beyond a narrow four-corner cabin approximation");
    for (auto& p:sidewall) p.pos[2]=.1f+(p.pos[2]-.1f)*.05f;
    check(!gt2view::cockpit_detail::ExtendedWindowFace(sidewall,shell,1.f),
          "a short external mirror never qualifies as a long side window");
    for (auto& p:sidewall) p.pos[1]=1.12f;
    check(gt2view::cockpit_detail::HeaderTrim(sidewall,shell,.2f) &&
          !gt2view::cockpit_detail::HeaderTrim(sidewall,shell,.8f) &&
          !gt2view::cockpit_detail::HeaderTrim(sidewall,shell,0.f),
          "sparse teal lettering on an upper opaque band stays solid while primary glazing remains eligible");
    auto internal=model;
    auto& internalLod=internal.lods[0];internalLod.vertices={{0,200,-8192,0},{0,2900,-8192,0},{0,2900,0,0},{0,200,0,0}};
    gt2::CarPolygon partition;partition.primCode=0x28;partition.vertex={0,1,2,3};partition.r=partition.g=partition.b=255;
    internalLod.polygons={partition};
    const auto internalMesh=gt2::BuildCarMesh(internal,options);
    check(gt2view::cockpit_detail::InteriorCentrePlane(internalMesh,fit),
          "a narrow untextured longitudinal chassis partition is recognized inside the closed car shell");
    const auto clippedPartition=gt2view::BuildCockpitBody(internal,texture,fit);
    check(!clippedPartition.vertices.empty() && std::all_of(clippedPartition.vertices.begin(),clippedPartition.vertices.end(),[&](const auto& p) {
        return p.pos[2]<=fit.windshieldZ+1e-6f && p.color[0]==1.f;
    }),"cockpit removes only the cabin part of an internal partition and preserves its original bonnet portion");
    auto outerPartition=internalMesh;
    for (auto& p:outerPartition) p.pos[0]=.65f;
    check(!gt2view::cockpit_detail::InteriorCentrePlane(outerPartition,fit),"an exterior side panel is never removed as an internal centre partition");
    for (auto& p:outerPartition) {p.pos[0]=0;p.textured=true;}
    check(!gt2view::cockpit_detail::InteriorCentrePlane(outerPartition,fit),"textured authored surfaces retain their geometry");
    auto raised=internal;
    raised.lods[0].vertices={{-4000,1600,-4096,0},{4000,1600,-4096,0},{4000,1600,8192,0},{-4000,1600,8192,0}};
    const auto raisedMesh=gt2::BuildCarMesh(raised,options);
    check(gt2view::cockpit_detail::InteriorRaisedFloor(raisedMesh,fit),
          "a wide raised retail floor hidden inside the body is replaced by the cockpit floor");
    const auto loweredFloor=gt2view::BuildCockpitBody(raised,texture,fit);
    bool keptFront=false,keptRear=false,emptyCabin=true;
    for (const auto& p:loweredFloor.vertices) {
        keptFront |= p.pos[2]<fit.windshieldZ-.01f;
        keptRear |= p.pos[2]>fit.rearWindowZ+.01f;
        emptyCabin &= p.pos[2]<=fit.windshieldZ+1e-6f || p.pos[2]>=fit.rearWindowZ-1e-6f;
    }
    check(keptFront && keptRear && emptyCabin,"internal-floor removal preserves original front and rear exterior portions");
    auto groundFloor=raisedMesh;
    for (auto& p:groundFloor) p.pos[1]=fit.floorY;
    check(!gt2view::cockpit_detail::InteriorRaisedFloor(groundFloor,fit),"the genuine lowest underbody floor is preserved");
    auto bulkhead=raisedMesh;
    for (size_t i=0;i<bulkhead.size();++i) {
        bulkhead[i].pos[2]=(fit.windshieldZ+fit.rearWindowZ)*.5f;
        bulkhead[i].pos[1]=(i%2)?fit.floorY:fit.windshieldY-.15f;
    }
    check(gt2view::cockpit_detail::InteriorChassisBulkhead(bulkhead,fit),
          "the vertical end of an internal raised chassis box is removed with its lid");
    for (auto& p:bulkhead) p.pos[2]=fit.windshieldZ-.1f;
    check(!gt2view::cockpit_detail::InteriorChassisBulkhead(bulkhead,fit),
          "an external chassis end ahead of the cabin is preserved");
    auto adjoining=raisedMesh;
    adjoining[2].pos[1]=fit.floorY;adjoining[4].pos[1]=fit.floorY;adjoining[5].pos[1]=fit.floorY;
    check(gt2view::cockpit_detail::SharesEdge(adjoining,raisedMesh),
          "the lid and wall of a chassis box share two unique original vertices");
    for (auto& p:adjoining) p.pos[2]+=.01f;
    check(!gt2view::cockpit_detail::SharesEdge(adjoining,raisedMesh),
          "a nearby unrelated chassis wall is not removed with a raised floor");
    auto roofTrim=raisedMesh;
    for (auto& p:roofTrim) {p.pos[0]*=.5f;p.pos[1]=fit.eye[1]+.06f;p.pos[2]=-.2f;}
    check(gt2view::cockpit_detail::UpperCabinTrim(roofTrim,fit),
          "an upper roof edge receives inner lining even when a roof scoop narrows the fitted header");
    for (auto& p:roofTrim) p.pos[2]=fit.rearWindowZ+.5f;
    check(!gt2view::cockpit_detail::UpperCabinTrim(roofTrim,fit),"a rear aero surface beyond the cabin keeps its exterior finish");
    auto roofFace=std::span(original).first(6);
    std::vector<gt2::CarMeshVertex> roof(roofFace.begin(),roofFace.end());
    for (auto& v:roof) { v.pos[1]=1.1f; v.pos[2]+=.5f; }
    auto roofFit=fit; roofFit.roofFrontY=roofFit.roofRearY=1.1f;
    check(gt2view::cockpit_detail::RoofPanel(roof,roofFit),
          "horizontal roof paint cannot create false window holes from teal decal colours");
    check(!gt2view::cockpit_detail::RoofPanel(roofFace,roofFit),
          "sloping windshield stays eligible for glass cutting");

    auto layered=model;
    layered.lods[0].polygons={window,window,bonnet};
    layered.lods[0].polygons[1].palette=1;
    layered.lods[0].vertices[0]={2048,3072,-2048,0};
    layered.lods[0].vertices[1]={2048,3072,0,0};
    layered.lods[0].vertices[2]={2048,4096,0,0};
    layered.lods[0].vertices[3]={2048,4096,-2048,0};
    auto layeredTexture=texture;
    std::fill(layeredTexture.indices.begin(),layeredTexture.indices.end(),uint8_t(2));
    for (int y=0;y<=8;++y) layeredTexture.indices[size_t(y)*256]=1;
    for (auto& paint:layeredTexture.paints) { paint.cluts[1][1]=0; paint.cluts[1][2]=0; }
    const auto layeredBody=gt2view::BuildCockpitBody(layered,layeredTexture,fit);
    check(layeredBody.vertices.size()==3 && layeredBody.exteriorPolygons==std::vector<size_t>{2},
          "coincident quad frame opens its dark side pane without changing the bonnet");

    auto decalModel=model;
    auto decal=window; decal.palette=1;
    decalModel.lods[0].polygons={window,decal,bonnet};
    auto decalTexture=texture;
    std::fill(decalTexture.indices.begin(),decalTexture.indices.end(),uint8_t(1));
    for (int y=2;y<7;++y) for (int x=2;x<7;++x) decalTexture.indices[size_t(y)*256+size_t(x)]=2;
    for (auto& paint:decalTexture.paints) {
        paint.cluts[0][2]=paint.cluts[0][1];
        paint.cluts[1][1]=0; paint.cluts[1][2]=color(24,4,4);
    }
    const auto decalBody=gt2view::BuildCockpitBody(decalModel,decalTexture,fit);
    const auto decalOriginal=gt2::BuildCarMesh(decalModel,options);
    check(decalBody.vertices.size()==3 && decalBody.windowTriangles==2 &&
          decalBody.exteriorPolygons==std::vector<size_t>{2},
          "a separate alpha decal over proven windshield glass never becomes an opaque interior triangle");
    check(decalOriginal.size()==15 && decalOriginal[6].palette==1,
          "the original exterior mesh retains its windshield decal");
    auto splitModel=decalModel;
    auto firstPane=window,secondPane=window;
    firstPane.primCode=secondPane.primCode=0x25;
    firstPane.vertex={0,1,3,0};firstPane.u={0,8,0,0};firstPane.v={0,0,8,0};
    secondPane.vertex={1,2,3,0};secondPane.u={8,8,0,0};secondPane.v={0,8,8,0};
    splitModel.lods[0].polygons={firstPane,secondPane,decal,bonnet};
    const auto splitBody=gt2view::BuildCockpitBody(splitModel,decalTexture,fit);
    check(splitBody.vertices.size()==3 && splitBody.windowTriangles==2,
          "a windshield decal can span multiple independently authored glass polygons");
    auto darkPaneTexture=decalTexture;
    for (auto& paint:darkPaneTexture.paints) paint.cluts[0][1]=color(2,2,2);
    const auto darkPaneBody=gt2view::BuildCockpitBody(decalModel,darkPaneTexture,fit);
    check(darkPaneBody.vertices.size()==3,
          "a predominantly dark windshield with a teal reflection also supports decal removal");
    auto opaqueTexture=decalTexture;
    for (auto& paint:opaqueTexture.paints) paint.cluts[1][1]=color(24,4,4);
    const auto opaqueBody=gt2view::BuildCockpitBody(decalModel,opaqueTexture,fit);
    check(opaqueBody.vertices.size()==9,"an opaque structural overlay is not mistaken for a transparent decal");
    auto separate=decalModel;
    auto& separateLod=separate.lods[0];
    for (size_t i=0;i<4;++i) {
        auto v=separateLod.vertices[i];v.z+=820;
        separateLod.polygons[1].vertex[i]=uint8_t(separateLod.vertices.size());
        separateLod.vertices.push_back(v);
    }
    const auto separateMesh=gt2::BuildCarMesh(separate,options);
    check(!gt2view::cockpit_detail::WindowOverlay(std::span(separateMesh).subspan(6,6),std::span(separateMesh).first(6)),
          "an alpha-trim panel away from the glass is not a windshield overlay");
}

inline bool CockpitForwardBlocked(const gt2view::CockpitBody& body, const gt2::CarTexture& texture,
                                  const gt2view::CockpitFit& fit, float back = 0) {
    const std::array<float, 3> eye{fit.eye[0], fit.eye[1], fit.eye[2] + back};
    for (size_t at = 0; at + 2 < body.vertices.size(); at += 3) {
        const auto* tri = body.vertices.data() + at;
        const float ax = tri[0].pos[0], ay = tri[0].pos[1];
        const float bx = tri[1].pos[0] - ax, by = tri[1].pos[1] - ay;
        const float cx = tri[2].pos[0] - ax, cy = tri[2].pos[1] - ay;
        const float determinant = bx * cy - by * cx;
        if (std::abs(determinant) < 1e-8f) continue;
        const float u = ((eye[0] - ax) * cy - (eye[1] - ay) * cx) / determinant;
        const float v = (bx * (eye[1] - ay) - by * (eye[0] - ax)) / determinant;
        if (u < 0 || v < 0 || u + v > 1) continue;
        const float weights[] = {1 - u - v, u, v};
        float z = 0, tu = 0, tv = 0;
        for (size_t k = 0; k < 3; ++k) {
            z += tri[k].pos[2] * weights[k]; tu += tri[k].texel[0] * weights[k]; tv += tri[k].texel[1] * weights[k];
        }
        if (z >= eye[2] - .005f) continue;
        if (!tri[0].textured) return true;
        const auto x = std::clamp(int(tu), 0, 255), y = std::clamp(int(tv), 0, 223);
        const auto index = texture.indices[size_t(y) * 256 + x];
        const auto palette = tri[0].palette;
        if (palette >= gt2view::kCockpitGlazingPalette && palette < gt2view::kCockpitLiningPalette &&
            (body.glassMasks[palette - gt2view::kCockpitGlazingPalette] & (1u << index))) continue;
        if (texture.paints.front().cluts[gt2view::CockpitSourcePalette(palette)][index]) return true;
    }
    return false;
}

template<class Check>
void CockpitCorpus(const char* discPath, const char* output, Check check) {
    gt2::DiscImage disc(discPath);
    gt2::GtfsVolume volume(disc);
    std::ofstream report(output);
    if (!report) throw std::runtime_error("cannot write cockpit corpus report");
    report << "car,windows,vertices,eye_x,eye_y,eye_z,roof_y,cowl_z,roof_front_z,roof_rear_z,open_top,blocked_vr,blocked_flat,cowl_y,left_x,left_y,left_z,mirror_y,mirror_z,side_opening_samples,old_side_overlap,side_overlap\n";
    size_t cars = 0, blocked = 0, noGlass = 0, sideBlocked = 0, oldSideBlocked = 0;
    for (const auto& entry : volume.Files()) {
        if (!entry.path.ends_with(".cdo.gz") && !entry.path.ends_with(".cdo")) continue;
        const auto model = gt2::ParseCarModel(volume.Read(entry));
        auto fit = gt2view::FitCockpit(model);
        if (!fit.valid) continue;
        auto texturePath = entry.path; texturePath.replace(texturePath.find(".cdo"), 4, ".cdp");
        const auto texture = gt2::ParseCarTexture(volume.Read(texturePath));
        const auto body = gt2view::BuildCockpitBody(model, texture, fit);
        const auto straightSill=fit;
        gt2view::FitCockpitSideSills(fit,body.windowOpenings);
        gt2view::RefineCockpitEye(fit, body.vertices, texture, body.glassMasks);
        gt2view::FitCockpitMirror(fit, model);
        size_t reflected = 0;
        for (size_t index : body.exteriorPolygons) {
            const auto& polygon = model.lods[0].polygons[index];
            if (polygon.renderFlags & 0x8000) reflected += polygon.IsQuad() ? 6 : 3;
        }
        check(body.vertices.size() + reflected <= 8192, entry.path + ": cockpit exterior reflections fit without overwriting the next car");
        check(!body.vertices.empty() && body.vertices.size() <= 8192, entry.path + ": body fits the reserved range");
        check(std::all_of(body.vertices.begin(), body.vertices.end(), [](const auto& vertex) {
            return std::isfinite(vertex.pos[0]) && std::isfinite(vertex.pos[1]) && std::isfinite(vertex.pos[2]);
        }), entry.path + ": finite body geometry");
        for (float axis : fit.eye) check(std::isfinite(axis), entry.path + ": finite driver position");
        const bool vr = CockpitForwardBlocked(body, texture, fit, .15f), flat = CockpitForwardBlocked(body, texture, fit, .33f);
        float sideOverlap=0,oldSideOverlap=0;
        size_t sideSamples=0;
        for (size_t at=0;at+2<body.windowOpenings.size();at+=3) {
            const auto triangle=std::span(body.windowOpenings).subspan(at,3);
            if (!gt2view::CockpitSideOpening(triangle,fit)) continue;
            const size_t side=triangle[0].pos[0]>=0;
            for (int i=0;i<=6;++i) for (int j=0;j<=6-i;++j) {
                std::array<float,3> p{};
                const float weights[]={float(i)/6,float(j)/6,float(6-i-j)/6};
                for (size_t k=0;k<3;++k) for (size_t axis=0;axis<3;++axis) p[axis]+=triangle[k].pos[axis]*weights[k];
                if (p[2]<fit.frontCowl[side][2] || p[2]>fit.rearCowl[side][2]) continue;
                sideOverlap=std::max(sideOverlap,gt2view::CockpitSideSillAt(fit,side,p[2])[1]-p[1]);
                oldSideOverlap=std::max(oldSideOverlap,gt2view::CockpitSideSillAt(straightSill,side,p[2])[1]-p[1]);
                ++sideSamples;
            }
        }
        check(sideOverlap<.001f,entry.path+": door-card boundary does not cross side-window openings");
        sideBlocked+=sideOverlap>.001f; oldSideBlocked+=oldSideOverlap>.001f;
        report << entry.path << ',' << body.windowTriangles << ',' << body.vertices.size() << ',' << fit.eye[0] << ',' << fit.eye[1] << ','
               << fit.eye[2] << ',' << fit.roofY << ',' << fit.windshieldZ << ',' << fit.roofFrontZ << ',' << fit.roofRearZ << ','
               << fit.openTop << ',' << vr << ',' << flat << ',' << fit.windshieldY << ',' << fit.frontCowl[0][0] << ','
               << fit.frontCowl[0][1] << ',' << fit.frontCowl[0][2] << ',' << fit.mirror[1] << ',' << fit.mirror[2] << ','
               << sideSamples << ',' << oldSideOverlap << ',' << sideOverlap << '\n';
        ++cars; blocked += vr || flat; noGlass += !body.windowTriangles;
    }
    check(cars > 0, "cockpit corpus visited car assets");
    std::cout << "COCKPIT cars=" << cars << " blocked_forward=" << blocked << " no_glass=" << noGlass
              << " blocked_side=" << sideBlocked << " old_blocked_side=" << oldSideBlocked << '\n';
    check(blocked == 0, "cockpit corpus has no opaque body across the default forward sightline");
}
