// Model-independent authored-cabin acceptance gate. A reviewed per-car contract
// supplies native seam paths, opaque window edges and the mirror's search volume.
#include "offscreen_car_audit.h"
#include "gt2view/authored_car.h"
#include "gt2export/png_writer.h"
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
using namespace gt2view;
using V=std::array<float,3>;
using J=gt2::json::Value;
static V Sub(V a,V b){for(size_t k=0;k<3;++k)a[k]-=b[k];return a;}
static V Add(V a,V b){for(size_t k=0;k<3;++k)a[k]+=b[k];return a;}
static V Mul(V a,float b){for(float& x:a)x*=b;return a;}
static float Dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static float Distance(V p,V a,V b,V c){
 const V ab=Sub(b,a),ac=Sub(c,a),ap=Sub(p,a);float d1=Dot(ab,ap),d2=Dot(ac,ap);
 if(d1<=0 && d2<=0)return std::sqrt(Dot(ap,ap));
 const V bp=Sub(p,b);float d3=Dot(ab,bp),d4=Dot(ac,bp);
 if(d3>=0 && d4<=d3)return std::sqrt(Dot(bp,bp));
 float vc=d1*d4-d3*d2;V q{};
 if(vc<=0 && d1>=0 && d3<=0)q=Add(a,Mul(ab,d1/(d1-d3)));
 else {const V cp=Sub(p,c);float d5=Dot(ab,cp),d6=Dot(ac,cp);
  if(d6>=0 && d5<=d6)return std::sqrt(Dot(cp,cp));
  float vb=d5*d2-d1*d6;
  if(vb<=0 && d2>=0 && d6<=0)q=Add(a,Mul(ac,d2/(d2-d6)));
  else {float va=d3*d6-d5*d4;
   if(va<=0 && d4>=d3 && d5>=d6)q=Add(b,Mul(Sub(c,b),(d4-d3)/((d4-d3)+(d5-d6))));
   else {float den=va+vb+vc;if(std::abs(den)<1e-20f)return 1000; q=Add(a,Add(Mul(ab,vb/den),Mul(ac,vc/den)));}
  }
 }
 V d=Sub(p,q);return std::sqrt(Dot(d,d));
}
static bool Matches(const std::string& name,const J& prefixes){for(size_t i=0;i<prefixes.Size();++i)if(name.starts_with(prefixes.At(i).AsString()))return true;return false;}
struct Result {J rows=J::Array();std::vector<std::string> errors;std::vector<V> probes;int mirrors=0;};
static Result Measure(const AuthoredCar& car,const J& contract){
 Result result;
 const auto& seams=contract.Require("seams");
 for(size_t si=0;si<seams.Size();++si){
  const auto& seam=seams.At(si);const auto& path=seam.Require("path");float worst=0;
  for(size_t j=1;j<path.Size();++j)for(int sample=0;sample<=16;++sample){
   const V a=AuthoredVector(path.At(j-1)),b=AuthoredVector(path.At(j));const V point=Add(a,Mul(Sub(b,a),float(sample)/16));
   float distance=1000;
   for(const auto& primitive:car.mesh.primitives)if(Matches(primitive.nodeName,seam.Require("nodes")))
    for(size_t t=0;t+2<primitive.indices.size();t+=3)distance=std::min(distance,Distance(point,primitive.vertices[primitive.indices[t]].position,primitive.vertices[primitive.indices[t+1]].position,primitive.vertices[primitive.indices[t+2]].position));
   worst=std::max(worst,distance);result.probes.push_back(point);
  }
  const bool pass=worst<=float(seam.DoubleAt("tolerance"));
  J row=J::Object();row.Set("name",J::String(seam.Require("name").AsString()));row.Set("worstMetres",J::Double(worst));row.Set("pass",J::Bool(pass));result.rows.Push(row);
  if(!pass)result.errors.push_back(seam.Require("name").AsString());
 }
 for(size_t wi=0;wi<contract.Require("windowFrames").Size();++wi){
  const auto name=contract.Require("windowFrames").At(wi).AsString();bool found=false,clean=true;
  for(const auto& p:car.mesh.primitives)if(p.nodeName==name){found=true;
   if(p.material>=0){const auto& m=car.mesh.materials[size_t(p.material)];clean&=!m.alphaBlend;
    if(m.baseColorImage>=0){const auto& im=car.mesh.images[size_t(m.baseColorImage)];for(size_t k=3;k<im.rgba.size();k+=4)if(im.rgba[k]!=255){clean=false;break;}}
   }
  }
  if(!found || !clean)result.errors.push_back("masked_or_missing_window_frame:"+name);
 }
 // Count actual wide connected mirror surfaces in the header volume, independent
 // of object names. A second stock card embedded in another mesh is still found.
 const auto& mr=contract.Require("mirror");const V lo=AuthoredVector(mr.Require("min")),hi=AuthoredVector(mr.Require("max"));
 std::vector<V> centres;
 for(const auto& p:car.mesh.primitives){
  std::vector<size_t> parent(p.vertices.size());std::iota(parent.begin(),parent.end(),0);
  auto find=[&](size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
  std::map<std::array<int,3>,size_t> welded;
  for(size_t i=0;i<p.vertices.size();++i){std::array<int,3> key{};for(size_t k=0;k<3;++k)key[k]=int(std::lround(p.vertices[i].position[k]*10000));auto [it,inserted]=welded.emplace(key,i);if(!inserted)parent[find(i)]=find(it->second);}
  for(size_t i=0;i+2<p.indices.size();i+=3){parent[find(p.indices[i+1])]=find(p.indices[i]);parent[find(p.indices[i+2])]=find(p.indices[i]);}
  std::map<size_t,std::vector<V>> groups;for(size_t i=0;i<p.vertices.size();++i)groups[find(i)].push_back(p.vertices[i].position);
  for(const auto& [id,points]:groups){(void)id;V mn{100,100,100},mx{-100,-100,-100};bool inside=true;
   for(const auto& v:points)for(size_t k=0;k<3;++k){mn[k]=std::min(mn[k],v[k]);mx[k]=std::max(mx[k],v[k]);inside&=v[k]>=lo[k] && v[k]<=hi[k];}
   if(!inside || mx[0]-mn[0]<float(mr.DoubleAt("minWidth")) || mx[1]-mn[1]<.025f)continue;
   V center=Mul(Add(mn,mx),.5f);bool duplicate=false;for(V old:centres){V d=Sub(old,center);duplicate|=Dot(d,d)<.0009f;}
   if(!duplicate)centres.push_back(center);
  }
 }
 result.mirrors=int(centres.size());if(result.mirrors!=1)result.errors.push_back("rear_view_mirror_count:"+std::to_string(result.mirrors));
 return result;
}
static V Semantic(const std::string& name){
 if(name.find("mirror")!=std::string::npos || name.find("_088_")!=std::string::npos)return {1,1,0};
 if(name.starts_with("Cabin_roll_cage"))return {0,.15f,1};
 if(name.starts_with("Cabin_") || name=="Racing_wheel")return {0,1,0};
 if(name.starts_with("R32_"))return {1,.3f,0};
 return {0,1,1};
}
int main(int argc,char** argv){try{
 if(argc!=4)throw std::runtime_error("authored-fit-audit manifest contract output");
 const auto manifest=gt2::json::ReadFile(argv[1]);auto car=ReadAuthoredCar(argv[1],manifest.Require("car").AsString());const auto contract=gt2::json::ReadFile(argv[2]);
 if(contract.Require("car").AsString()!=manifest.Require("car").AsString())throw std::runtime_error("fit contract belongs to another car");
 const std::filesystem::path out=argv[3];std::filesystem::create_directories(out);auto result=Measure(car,contract);
 // Mutation tests prove that the acceptance criteria reject the regressions.
 J mutations=J::Array();
 if(result.errors.empty())for(int mutation=0;mutation<4;++mutation){
  auto changed=car;
  if(mutation<2){const auto& edit=contract.Require("negativeTranslations").At(size_t(mutation));const V delta=AuthoredVector(edit.Require("delta"));for(auto& p:changed.mesh.primitives)if(Matches(p.nodeName,edit.Require("nodes")))for(auto& v:p.vertices)v.position=Add(v.position,delta);}
  else if(mutation==2){const auto name=contract.Require("mirror").Require("node").AsString();auto it=std::find_if(changed.mesh.primitives.begin(),changed.mesh.primitives.end(),[&](const auto& p){return p.nodeName==name;});if(it==changed.mesh.primitives.end())throw std::runtime_error("mirror mutation needs annotated mirror node");auto duplicate=*it;for(auto& v:duplicate.vertices)v.position[1]-=.075f;changed.mesh.primitives.push_back(duplicate);}
  else {const auto name=contract.Require("windowFrames").At(0).AsString();for(auto& p:changed.mesh.primitives)if(p.nodeName==name && p.material>=0)changed.mesh.materials[size_t(p.material)].alphaBlend=true;}
  const bool rejected=!Measure(changed,contract).errors.empty();mutations.Push(J::Bool(rejected));if(!rejected)result.errors.push_back("mutation_not_rejected:"+std::to_string(mutation));
 }
 Device device;VkContext context(device.existing);constexpr uint32_t width=800,height=600;
 VkSceneRenderer renderer(context,{width,height},VK_FORMAT_R8G8B8A8_UNORM);renderer.CreateStereoTarget({width,height},VK_FORMAT_R8G8B8A8_UNORM,true);
 for(auto& im:car.mesh.images)for(size_t i=0;i<im.rgba.size();i+=4)im.rgba[i]=im.rgba[i+1]=im.rgba[i+2]=255;
 for(auto& p:car.mesh.primitives){auto material=p.material>=0?car.mesh.materials[size_t(p.material)]:gt2::GltfMaterial{};const V color=Semantic(p.nodeName);for(size_t k=0;k<3;++k)material.baseColorFactor[k]=color[k];material.emissive=true;
  p.material=int(car.mesh.materials.size());car.mesh.materials.push_back(material);for(auto& v:p.vertices)v.color={1,1,1,1};
 }
 std::vector<uint32_t> pages;uint32_t cursor=SceneRenderer::kModelSetTexelBase;
 for(const auto& im:car.mesh.images){std::vector<uint32_t> texels(size_t(im.width)*size_t(im.height));for(size_t i=0;i<texels.size();++i){const auto* p=im.rgba.data()+i*4;texels[i]=uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}pages.push_back(cursor);renderer.UploadExternalTexture(cursor,uint32_t(texels.size()),texels.data());cursor+=uint32_t(texels.size());}
 const auto geometry=BuildAuthoredGeometry(car,pages);const uint32_t base=SceneRenderer::kModelSetVertexBase;renderer.SetVertices(base,geometry.vertices);
 std::vector<DrawItem> draws;for(const auto& range:geometry.ranges){if(range.alpha || (range.lampStates && !(range.lampStates&1u)))continue;DrawItem d;d.firstVertex=base+range.first;d.vertexCount=range.count;d.mvp[0]=d.mvp[5]=d.mvp[10]=d.mvp[15]=1;d.space=kSpaceWorld;draws.push_back(d);}
 renderer.clearColor[0]=renderer.clearColor[2]=1;renderer.clearColor[1]=0;
 J renderRows=J::Array();size_t checked=0,leaks=0;
 const auto& cameras=contract.Require("views");
 for(size_t ci=0;ci<cameras.Size();++ci){const auto& row=cameras.At(ci);V eye=Add(car.fit.eye,AuthoredVector(row.Require("offset")));StereoViews views;std::array<std::array<float,16>,2> matrices;
  for(size_t e=0;e<2;++e){V at=eye;at[0]+=e?.032f:-.032f;auto vp=View(at,float(row.DoubleAt("yaw")),float(row.DoubleAt("pitch")));for(size_t c=0;c<4;++c)vp[c*4]*=float(height)/width;matrices[e]=vp;std::copy(vp.begin(),vp.end(),views.worldVP[e]);}renderer.SetStereoViews(views);
  const auto image=renderer.CaptureStereoForAudit(draws,draws.size());gt2::WritePngRgba((out/("fit-colour-"+std::to_string(ci)+".png")).string(),int(width),int(height*2),image);
  size_t viewChecked=0,viewLeaks=0;
  for(size_t e=0;e<2;++e)for(V point:result.probes){const auto& m=matrices[e];float w=m[15]+m[3]*point[0]+m[7]*point[1]+m[11]*point[2];if(w<=.08f)continue;
   const float x=(m[12]+m[0]*point[0]+m[4]*point[1]+m[8]*point[2])/w,y=(m[13]+m[1]*point[0]+m[5]*point[1]+m[9]*point[2])/w;
   const int px=int((x*.5f+.5f)*width),py=int((y*.5f+.5f)*height);if(px<4 || py<4 || px>=int(width)-4 || py>=int(height)-4)continue;
   // A 3x3 kernel tolerates only raster-edge rounding, not a visible strip of sky.
   bool solid=false;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){size_t k=(e*width*height+size_t(py+dy)*width+size_t(px+dx))*4;solid|=!(image[k]>240 && image[k+1]<16 && image[k+2]>240);}
   ++viewChecked;if(!solid)++viewLeaks;
  }
  checked+=viewChecked;leaks+=viewLeaks;J rr=J::Object();rr.Set("view",J::Int(int64_t(ci)));rr.Set("checked",J::Int(int64_t(viewChecked)));rr.Set("openSeamPixels",J::Int(int64_t(viewLeaks)));renderRows.Push(rr);
 }
 if(!checked || leaks)result.errors.push_back("rendered_seam_leaks:"+std::to_string(leaks));
 J report=J::Object();report.Set("pass",J::Bool(result.errors.empty()));report.Set("seams",result.rows);report.Set("mirrorCount",J::Int(result.mirrors));report.Set("mutationRejections",mutations);report.Set("colourViews",renderRows);J errors=J::Array();for(const auto& e:result.errors){errors.Push(J::String(e));std::cerr<<e<<'\n';}report.Set("errors",errors);gt2::json::WriteFile((out/"fit-checks.json").string(),report);
 std::cout<<"Fit gate: "<<(result.errors.empty()?"PASS":"FAIL")<<", "<<result.mirrors<<" mirror, "<<checked<<" stereo seam probes, "<<leaks<<" leaks\n";return result.errors.empty()?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
