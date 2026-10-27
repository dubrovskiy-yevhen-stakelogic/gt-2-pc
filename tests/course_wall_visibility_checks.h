#pragma once
#include "gt2view/course_wall_visibility.h"
template<class Check> void CourseWallVisibilityChecks(Check require) {
    gt2::Track track;track.chunks.resize(2);
    auto& road=track.chunks[1];road.renderList={1};road.road.vertices={{-640,-640,0},{-640,640,0},{640,640,0},{640,-640,0}};
    gt2::TrackPolygon p;p.primCode=0x2c;p.vertex={0,1,2,3};road.road.polygons.push_back(p);
    road.surfaceGrid.cells[0]={0};track.chunks[0].renderList={0};
    const std::array<std::array<float,3>,4> wall{{{-5,-2,0},{-5,2,0},{5,2,0},{5,-2,0}}};
    const gt2view::CourseWallVisibility index(track);
    require(index.HiddenIntersection(0,wall,true,false),"unlisted wall crosses a foreign contact surface");
    require(!index.HiddenIntersection(0,wall,true,true),"physical contact polygon is preserved");
    auto bridge=wall;for(auto& v:bridge)v[1]+=5;
    require(!index.HiddenIntersection(0,bridge,true,false),"overhead bridge wall is preserved");
    auto edge=wall;for(auto& v:edge)v[1]+=2;
    require(!index.HiddenIntersection(0,edge,true,false),"wall resting on road is preserved");
    auto outside=wall;for(auto& v:outside)v[2]+=20;
    require(!index.HiddenIntersection(0,outside,true,false),"non-overlapping wall is preserved");
    // Track vertices store x,z,y. The upper rail has a different material,
    // duplicated vertices and touches the crossing wall only at a T junction.
    auto& source=track.chunks[0].road;
    source.vertices={{-320,0,-128},{-320,0,128},{320,0,128},{320,0,-128},
        {320,0,128},{320,0,192},{640,0,192},{640,0,128}};
    source.polygons.push_back(p);p.vertex={4,5,6,7};p.uvIndex=1;source.polygons.push_back(p);
    track.uvTable.resize(2);track.uvTable[1].nearSet.tpage=17;
    std::vector<size_t> conflicts;const auto mask=gt2view::CourseWallVisibility(track).Classify(track,0,&conflicts);
    require(mask.size()==2 && mask[0] && mask[1],"whole wall includes raised rail across material and vertex split");
    require(conflicts==std::vector<size_t>{1},"visibility restriction records the conflicting road only");
    road.renderList.push_back(0);
    require(!gt2view::CourseWallVisibility(track).HiddenIntersection(0,wall,true,false),"authored co-visible wall is preserved");
    road.renderList.clear();
    require(!gt2view::CourseWallVisibility(track).HiddenIntersection(0,wall,true,false),"absent visibility metadata cannot hide geometry");
}
