#pragma once
#include "gt2formats/track.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <span>
#include <vector>

namespace gt2view {
// Some cliff tiles have a two-texel cutout fringe intended for a silhouette.
// At a shared course edge that fringe exposes the sky through a closed mesh.
// Inset only that joined edge; free silhouettes and interior transparency stay intact.
inline std::vector<std::vector<gt2::TrackUvSet>> CourseTextureSeams(
    const gt2::Track& track, std::span<const uint16_t> words) {
    using Point = std::array<int32_t,3>;
    using Edge = std::array<Point,2>;
    auto edgeOf = [](const gt2::TrackChunk& chunk, const gt2::TrackPolygon& p, size_t i) {
        Edge edge;
        for (size_t k=0;k<2;++k) {
            const auto w=gt2::TrackVertexToWorld(chunk,chunk.road.vertices[p.vertex[(i+k)%(p.IsQuad()?4:3)]]);
            for(size_t a=0;a<3;++a) edge[k][a]=int32_t(std::lround(w[a]*64));
        }
        if(edge[1]<edge[0]) std::swap(edge[0],edge[1]);
        return edge;
    };
    std::map<Edge,unsigned> edges;
    for(const auto& chunk:track.chunks) for(const auto& p:chunk.road.polygons)
        if(!(p.primCode&2)) for(size_t i=0;i<(p.IsQuad()?4u:3u);++i) ++edges[edgeOf(chunk,p,i)];
    auto texel=[&](const gt2::TrackUvSet& uv,int u,int v) {
        auto word=[&](size_t x,size_t y) { const size_t at=y*1024+x; return at<words.size()?words[at]:uint16_t(0); };
        const unsigned depth=(uv.tpage>>7)&3, x=(uv.tpage&15)*64, y=((uv.tpage>>4)&1)*256;
        if(depth>2) return uint16_t(0);
        const uint16_t packed=word(x+(u>>(2-depth)),y+v);
        if(depth==2) return packed;
        const unsigned bits=depth==0?4:8, index=(packed>>((u&((1<<(2-depth))-1))*bits))&((1<<bits)-1);
        return word((uv.clut&63)*16+index,uv.clut>>6);
    };
    std::map<std::pair<unsigned,unsigned>,std::array<int,4>> candidates;
    std::vector<std::vector<gt2::TrackUvSet>> result(track.chunks.size());
    for(size_t ci=0;ci<track.chunks.size();++ci) {
        const auto& chunk=track.chunks[ci];
        for(const auto& p:chunk.road.polygons) {
            gt2::TrackUvSet uv=p.IsTextured()?track.uvTable[p.uvIndex].nearSet:gt2::TrackUvSet{};
            if(p.IsTextured() && !(p.primCode&2) && words.size()>=1024*512) {
                const unsigned n=p.IsQuad()?4:3;
                const int lo[2]={*std::min_element(uv.u.begin(),uv.u.begin()+n),*std::min_element(uv.v.begin(),uv.v.begin()+n)};
                const int hi[2]={*std::max_element(uv.u.begin(),uv.u.begin()+n),*std::max_element(uv.v.begin(),uv.v.begin()+n)};
                auto [at,inserted]=candidates.try_emplace({p.uvIndex,n});
                if(inserted && hi[0]-lo[0]>=31 && hi[1]-lo[1]>=31) {
                    unsigned holes=0; std::array<int,4> inset{};
                    for(int v=lo[1];v<=hi[1];++v) for(int u=lo[0];u<=hi[0];++u) if(!texel(uv,u,v)) {
                        ++holes;
                        const int distance[4]={u-lo[0],hi[0]-u,v-lo[1],hi[1]-v};
                        for(int s=0;s<4;++s) inset[s]=std::max(inset[s],distance[s]+1);
                    }
                    if(holes && holes*50<unsigned((hi[0]-lo[0]+1)*(hi[1]-lo[1]+1)))
                        for(int s=0;s<4;++s) if(inset[s]<=2) at->second[s]=inset[s];
                }
                const auto original=uv;
                for(int s=0;s<4;++s) if(const int inset=at->second[s]) {
                    const int axis=s/2, boundary=(s&1)?hi[axis]:lo[axis];
                    const auto& coords=axis?original.v:original.u;
                    bool joined=false;
                    for(unsigned i=0;i<n;++i) if(coords[i]==boundary && coords[(i+1)%n]==boundary && edges[edgeOf(chunk,p,i)]==2) joined=true;
                    if(joined) {
                        auto& adjusted=axis?uv.v:uv.u;
                        for(unsigned i=0;i<n;++i) if(coords[i]==boundary) adjusted[i]=uint8_t(boundary+((s&1)?-inset:inset));
                    }
                }
            }
            result[ci].push_back(uv);
        }
    }
    return result;
}
}
