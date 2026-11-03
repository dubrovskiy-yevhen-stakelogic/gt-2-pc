#pragma once
#include "gt2view/model_glazing.h"
#include "gt2view/native_cockpit_fit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>
namespace gt2view {
// Axis visibility against the unmodified source, including original glazing
// carriers. An outside body panel is not disposable furniture merely because
// a fitted passenger volume crosses it. Wheels and reflection carriers cannot
// provide enclosure evidence. This deliberately does not classify glazing.
class SourceSurface167 {
 using P=std::array<float,3>;
 using T=std::array<P,3>;
 static constexpr int N=32;
 struct Coverage {int materialIndex=-1;std::array<std::array<float,2>,3> uv{};const gt2::GltfMaterial* material=nullptr;const gt2::PngImage* image=nullptr;};
 std::vector<Coverage> coverage;
 std::vector<T> triangles;
 bool covered(size_t index,float u,float v)const {
  const auto& c=coverage[index];if(!c.image || !c.material)return true;
  const auto& m=*c.material;const auto& image=*c.image;
  float uv[2]{};for(size_t k=0;k<2;++k)uv[k]=c.uv[0][k]+u*(c.uv[1][k]-c.uv[0][k])+v*(c.uv[2][k]-c.uv[0][k]);
  const auto coord=[](int a,int size,bool clamp){if(clamp)return std::clamp(a,0,size-1);return (a%size+size)%size;};
  const int x=int(std::floor(uv[0]*image.width-.5f)),y=int(std::floor(uv[1]*image.height-.5f));
  for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
   const auto alpha=image.rgba[(size_t(coord(y+dy,image.height,m.clampV))*image.width+size_t(coord(x+dx,image.width,m.clampU)))*4+3];
   if(NativeTextureAlphaDiscard(m,alpha))continue;
   if(alpha==0 && (m.alphaBlend || m.nativeGt4Type<0))continue;
   return true;
  }
  return false;
 }
 std::array<std::array<std::vector<size_t>,N*N>,3> grid;
 P lo{1e20f,1e20f,1e20f},hi{-1e20f,-1e20f,-1e20f};
 float epsilon;
 int cell(float x,size_t axis)const{return std::clamp(int((x-lo[axis])/std::max(1e-6f,hi[axis]-lo[axis])*N),0,N-1);}
 bool exposed(const P& p)const {
  for(size_t axis=0;axis<3;++axis){const size_t a=(axis+1)%3,b=(axis+2)%3;
   bool negative=axis==1 && p[1]>lo[1]+epsilon,positive=false;
   for(size_t index:grid[axis][size_t(cell(p[a],a)*N+cell(p[b],b))]){
    const auto& t=triangles[index];
    const float ax=t[1][a]-t[0][a],ay=t[1][b]-t[0][b],bx=t[2][a]-t[0][a],by=t[2][b]-t[0][b];
    const float det=ax*by-ay*bx;if(std::abs(det)<1e-12f)continue;
    const float x=p[a]-t[0][a],y=p[b]-t[0][b],u=(x*by-y*bx)/det,v=(ax*y-ay*x)/det;
    if(u<-.00001f || v<-.00001f || u+v>1.00001f || !covered(index,u,v))continue;
    const float d=t[0][axis]+u*(t[1][axis]-t[0][axis])+v*(t[2][axis]-t[0][axis])-p[axis];
    negative=negative || d < -epsilon;positive=positive || d > epsilon;
    if(negative && positive)break;
   }
   if(!negative || !positive)return true;
  }
  return false;
 }
 std::map<int,std::vector<ModelGlazingOpening>> windowWitnesses;
 bool insideWindow(const P& p,const ModelGlazingOpening& w)const {
  float d=0;for(size_t k=0;k<3;++k)d+=(p[k]-w.points[0][k])*w.normal[k];
  if(std::abs(d)>epsilon)return false;
  for(const auto& e:w.edges)if(e[0]*p[0]+e[1]*p[1]+e[2]*p[2]>e[3]+epsilon*std::sqrt(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]))return false;
  return true;
 }
 public:
 void SetWindowWitnesses(std::span<const ModelGlazingOpening> windows) {
  windowWitnesses.clear();for(const auto& w:windows)if(!w.reflectionOnly)windowWitnesses[w.sourceMaterial].push_back(w);
 }
 // An opening from one pass cannot override opaque coverage of a different
 // original pass at the same position. Check each pass against its OWN glass.
 bool WindowUnoccluded(const ModelGlazingOpening& opening)const {
  size_t axis=0;for(size_t k=1;k<3;++k)if(std::abs(opening.normal[k])>std::abs(opening.normal[axis]))axis=k;
  const size_t a=(axis+1)%3,b=(axis+2)%3;P center{};
  for(const auto& p:opening.points)for(size_t k=0;k<3;++k)center[k]+=p[k]/3;
  for(size_t sample=0;sample<4;++sample){P p=center;
   if(sample)for(size_t k=0;k<3;++k)p[k]=center[k]*.6f+opening.points[sample-1][k]*.4f;
   for(size_t index:grid[axis][size_t(cell(p[a],a)*N+cell(p[b],b))]){
    const auto& c=coverage[index];if(!c.material || !c.material->nativeDepthWrite)continue;
    const auto& t=triangles[index];
    const double ax=t[1][a]-t[0][a],ay=t[1][b]-t[0][b],bx=t[2][a]-t[0][a],by=t[2][b]-t[0][b];
    const double det=ax*by-ay*bx;if(std::abs(det)<1e-12)continue;
    const double x=p[a]-t[0][a],y=p[b]-t[0][b],u=(x*by-y*bx)/det,w=(ax*y-ay*x)/det;
    if(u<0 || w<0 || u+w>1 || !covered(index,float(u),float(w)))continue;
    const double height=t[0][axis]+u*(t[1][axis]-t[0][axis])+w*(t[2][axis]-t[0][axis]);
    if(std::abs(height-p[axis])>epsilon)continue;
    bool ownGlass=false;const auto found=windowWitnesses.find(c.materialIndex);
    if(found!=windowWitnesses.end())for(const auto& window:found->second)if(insideWindow(p,window)){ownGlass=true;break;}
    if(!ownGlass)return false;
   }
  }
  return true;
 }
 explicit SourceSurface167(const gt2::GltfMesh& mesh,float scale,std::span<const ModelGlazingOpening> glazing={}):epsilon(.0002f/std::max(.0001f,scale)) {
  for(const auto& primitive:mesh.primitives){
   const auto* m=primitive.material>=0?&mesh.materials.at(size_t(primitive.material)):nullptr;
   if(m && (m->reflection || m->wheelAttachment>=0 || m->lampStates))continue;
   for(size_t i=0;i+2<primitive.indices.size();i+=3){T t;
    for(size_t j=0;j<3;++j){t[j]=primitive.vertices.at(primitive.indices[i+j]).position;
     for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],t[j][k]);hi[k]=std::max(hi[k],t[j][k]);}}
    Coverage c;c.material=m;c.materialIndex=primitive.material;
    if(primitive.hasUvs && m && m->baseColorImage>=0)c.image=&mesh.images.at(size_t(m->baseColorImage));
    for(size_t j=0;j<3;++j)c.uv[j]=primitive.vertices.at(primitive.indices[i+j]).uv;
    triangles.push_back(t);coverage.push_back(c);
   }
  }
  // Only actual authored apertures, not an entire alpha-zero carrier,
  // close the shell while testing whether furniture is truly enclosed.
  for(const auto& opening:glazing)if(!opening.reflectionOnly){triangles.push_back(opening.points);coverage.emplace_back();}
  for(size_t index=0;index<triangles.size();++index)for(size_t axis=0;axis<3;++axis){
   const auto& t=triangles[index];const size_t a=(axis+1)%3,b=(axis+2)%3;
   const int x0=cell(std::min({t[0][a],t[1][a],t[2][a]}),a),x1=cell(std::max({t[0][a],t[1][a],t[2][a]}),a);
   const int y0=cell(std::min({t[0][b],t[1][b],t[2][b]}),b),y1=cell(std::max({t[0][b],t[1][b],t[2][b]}),b);
   for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)grid[axis][size_t(x*N+y)].push_back(index);
  }
 }

 // Audit-only original opaque silhouette. Native BLEND draws do not always
 // write the renderer depth buffer even where their texture is fully opaque.
 // Keep their actual UV coverage, minus only their own authored apertures.
 std::vector<gt2::GltfVertex> OpaqueTrianglesForAudit(const gt2::GltfMesh& mesh)const {
  std::vector<gt2::GltfVertex> result;std::map<int,ModelWindowHoles> masks;
  for(const auto& part:mesh.primitives){
   if(part.material<0)continue;const auto& m=mesh.materials.at(size_t(part.material));
   if(m.reflection || m.wheelAttachment>=0 || m.lampStates || !m.nativeDepthWrite)continue;
   const bool textured=part.hasUvs && m.baseColorImage>=0;
   if(textured && !masks.contains(part.material)){
    const auto& image=mesh.images.at(size_t(m.baseColorImage));std::vector<uint8_t> mask(image.rgba.size()/4);
    for(size_t k=0;k<mask.size();++k){const auto alpha=image.rgba[k*4+3];mask[k]=uint8_t(alpha>=254 && !NativeTextureAlphaDiscard(m,alpha));}
    masks.emplace(part.material,ExactModelMask(image,std::move(mask)));
   }
   const auto own=windowWitnesses.find(part.material);
   for(size_t at=0;at+2<part.indices.size();at+=3){
    const std::array<gt2::GltfVertex,3> triangle{part.vertices.at(part.indices[at]),part.vertices.at(part.indices[at+1]),part.vertices.at(part.indices[at+2])};
    if(std::any_of(triangle.begin(),triangle.end(),[&](const auto& v){return v.color[3]*m.baseColorFactor[3]<.999f;}))continue;
    auto covered=textured?GlazingInside(triangle,masks.at(part.material),m.clampU,m.clampV):std::vector<gt2::GltfVertex>(triangle.begin(),triangle.end());
    for(size_t k=0;k+2<covered.size();k+=3){const std::span<const gt2::GltfVertex> face(covered.data()+k,3);
     if(!Preserve(face))continue; // Exterior opaque witness excludes enclosed old furniture.
     if(own==windowWitnesses.end()){result.insert(result.end(),face.begin(),face.end());continue;}
     const auto kept=CutGlazingLayers(face,own->second,epsilon,false,part.material);result.insert(result.end(),kept.begin(),kept.end());
    }
   }
  }
  return result;
 }
 bool Preserve(std::span<const gt2::GltfVertex> triangle)const {
  if(triangles.empty() || triangle.size()!=3)return false;
  P center{};for(const auto& v:triangle)for(size_t k=0;k<3;++k)center[k]+=v.position[k]/3;
  if(!exposed(center))return false;
  for(const auto& v:triangle){P point{};for(size_t k=0;k<3;++k)point[k]=center[k]*.6f+v.position[k]*.4f;
   if(!exposed(point))return false;
  }
  return true;
 }
};
}
