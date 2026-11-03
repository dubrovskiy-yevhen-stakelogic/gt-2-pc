// Independent source-aperture/depth regression and translucent layer review.
#include "cockpit_fragment_views.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
 if(argc<5 || !std::getenv("GT2_AUDIT_COCKPIT") || !std::getenv("GT2_AUDIT_WHEEL_COLOR"))throw std::runtime_error("audit flags; disc pack output ids required");
 const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
 const auto base=std::filesystem::absolute(std::getenv("GT2_AUDIT_WORK_ROOT")?std::getenv("GT2_AUDIT_WORK_ROOT"):"work").lexically_normal();
 const auto rel=out.lexically_relative(base);if(rel.empty() || rel=="." || *rel.begin()=="..")throw std::runtime_error("output outside work root");
 std::filesystem::create_directories(out);
 gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
 Device device;VkContext context(device.existing);constexpr uint32_t size=512;
 VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);RenderOptions options;options.msaa=1;options.smoothTextures=true;
 renderer.SetOptions(options);renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
 const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};size_t cars=0,eyeViews=0,flagged=0;
 std::ofstream csv(out/"intrusions.csv");csv<<"car,native,pose,eye,visible_window_pixels,generated_window_intrusion,green_outside_source,largest_component,structure_window_intrusion\n";
 for(int arg=4;arg<argc;++arg){const std::string id=argv[arg];const auto dest=out/id;std::filesystem::create_directories(dest);
  SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("missing car "+id);
  const auto fit=assets.SlotCockpitFit(slot);const bool native=assets.HasNativeGt4Interior(slot);ProceduralCockpit cabin(renderer);
  std::vector<DrawItem> source,generated,mirrors,original,withoutWheel;assets.AppendCockpitBodyItems(source,slot,identity,0);
  assets.AppendCarItems(original,slot,identity,0,0,true); for(auto& d:original){d.space=kSpaceWorld;d.cabinDepth=true;} SortModelTransparency(original);
  cabin.Append(generated,fit,identity,0,80,3500,3,true,0,gt2game::CockpitSeatBack({}),nullptr,!native);
  cabin.Append(withoutWheel,fit,identity,0,80,3500,3,false,0,gt2game::CockpitSeatBack({}),nullptr,!native);
  cabin.AppendMirror(mirrors,identity);assets.AppendSideMirrorItems(mirrors,slot,identity);
  for(auto* draws:{&source,&generated,&mirrors,&withoutWheel}){for(auto& d:*draws)d.space=kSpaceWorld;SortModelTransparency(*draws);}
  gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,{});
  std::vector<SceneVertex> windows;
  // Captured before cabin replacement; never derive expected windows from
  // the generated sill, which is precisely the geometry under test.
  for(const auto& q:assets.ExposedWindowTrianglesForAudit(slot)){SceneVertex v{};std::copy(q.begin(),q.end(),v.pos);v.color[0]=v.color[1]=v.color[2]=1;windows.push_back(v);}
  // This audit draws only the GT4 model and generated cabin, both above
  // kNativeUiVertexBase. The unused track range is scratch, bounded by the
  // first persistent renderer allocation, including an optional blocker.
  if(windows.size()*(std::getenv("GT2_AUDIT_INJECT_WINDOW_BLOCKER")?2u:1u)>=SceneRenderer::kNativeUiVertexBase)throw std::runtime_error("aperture scratch budget exceeded "+id);
  renderer.SetVertices(0,windows);DrawItem aperture;aperture.vertexCount=uint32_t(windows.size());aperture.space=kSpaceWorld;aperture.cabinDepth=true;std::copy_n(identity,16,aperture.mvp);
  if(std::getenv("GT2_AUDIT_INJECT_WINDOW_BLOCKER")){
   auto blocker=windows;
   for(auto& v:blocker)for(size_t k=0;k<3;++k)v.pos[k]=camera.eye[k]+.8f*(v.pos[k]-camera.eye[k]);
   renderer.SetVertices(uint32_t(windows.size()),blocker);auto draw=aperture;draw.firstVertex=uint32_t(windows.size());generated.push_back(draw);withoutWheel.push_back(draw);
  }
  if(std::getenv("GT2_AUDIT_ENVELOPE_SELF_CONTROL")){generated.clear();withoutWheel.clear();}
  std::vector<SceneVertex> opaque;for(const auto& q:assets.OpaqueWitnessTrianglesForAudit(slot)){SceneVertex v{};std::copy(q.begin(),q.end(),v.pos);v.color[0]=v.color[1]=v.color[2]=1;opaque.push_back(v);}
  auto witness=aperture;witness.firstVertex=uint32_t(windows.size()*2);witness.vertexCount=uint32_t(opaque.size());
  if(witness.firstVertex+opaque.size()>=SceneRenderer::kNativeUiVertexBase)throw std::runtime_error("original opaque witness scratch budget exceeded");
  renderer.SetVertices(witness.firstVertex,opaque);const std::vector<DrawItem> witnessDraws=opaque.empty()?std::vector<DrawItem>{}:std::vector<DrawItem>{witness};

  // Keep seats in normal/translucent images and raw projection counts.
  // Independently inspect enclosure without legitimate seat occlusion.
  const auto seatRange=cabin.SeatRangeForAudit();std::vector<DrawItem> structure;
  const uint32_t seatStart=SceneRenderer::kCockpitVertexBase+seatRange[0],seatEnd=SceneRenderer::kCockpitVertexBase+seatRange[1];
  for(const auto& d:withoutWheel){const uint32_t end=d.firstVertex+d.vertexCount;
   if(end<=seatStart || d.firstVertex>=seatEnd || seatStart==seatEnd){structure.push_back(d);continue;}
   if(d.firstVertex<seatStart){auto part=d;part.vertexCount=seatStart-d.firstVertex;structure.push_back(part);}
   if(end>seatEnd){auto part=d;part.firstVertex=seatEnd;part.vertexCount=end-seatEnd;structure.push_back(part);}
  }
  std::vector<DrawItem> combined=source;combined.insert(combined.end(),generated.begin(),generated.end());combined.insert(combined.end(),mirrors.begin(),mirrors.end());SortModelTransparency(combined);
  for(const auto& pose:cockpit_fragment::poses){StereoViews stereo;
   for(int eye=0;eye<2;++eye){const auto vp=cockpit_fragment::Matrix({camera.eye[0],camera.eye[1],camera.eye[2]},pose,eye);std::copy(vp.begin(),vp.end(),stereo.worldVP[eye]);}
   renderer.SetStereoViews(stereo);renderer.clearColor[0]=0;renderer.clearColor[1]=renderer.clearColor[2]=1;
   const auto normal=renderer.CaptureStereoForAudit(combined,combined.size());
   const auto sourceColor=renderer.CaptureStereoForAudit(source,source.size());
   const auto generatedColor=renderer.CaptureStereoForAudit(generated,generated.size());
   const auto sourceDepth=renderer.CaptureStereoDepthForAudit(source,source.size());
   auto originalDepth=renderer.CaptureStereoDepthForAudit(original,original.size());
    const auto opaqueDepth=renderer.CaptureStereoDepthForAudit(witnessDraws,witnessDraws.size());
    for(size_t pixel=0;pixel<originalDepth.size();++pixel)originalDepth[pixel]=std::max(originalDepth[pixel],opaqueDepth[pixel]);
   const auto generatedDepth=renderer.CaptureStereoDepthForAudit(generated,generated.size());
   const auto structureDepth=renderer.CaptureStereoDepthForAudit(structure,structure.size());
   const auto windowDepth=renderer.CaptureStereoDepthForAudit(windows.empty()?std::vector<DrawItem>{}:std::vector<DrawItem>{aperture},windows.empty()?0:1);
   if(windowDepth.size()!=size_t(size)*size*2 || sourceDepth.size()!=windowDepth.size() || generatedDepth.size()!=windowDepth.size())throw std::runtime_error("invalid depth readback");
   auto overlay=normal,marked=normal,structureMarked=normal;std::vector<uint8_t> visible(windowDepth.size()),intrusion(windowDepth.size());
   for(size_t pixel=0;pixel<windowDepth.size();++pixel){const size_t at=pixel*4;
    for(float depth:{windowDepth[pixel],sourceDepth[pixel],generatedDepth[pixel]})if(!std::isfinite(depth)||depth<0||depth>1)throw std::runtime_error("invalid depth");
    const auto occupied=[&](const auto& color){return !(color[at]<5 && color[at+1]>250 && color[at+2]>250);};
    const bool pink=occupied(sourceColor),green=occupied(generatedColor);
    // Equal 40% contributions keep either layer visible through the other.
    overlay[at]=uint8_t(32+(pink?100:0));overlay[at+1]=uint8_t(32+(green?100:0));overlay[at+2]=uint8_t(32+(pink?100:0));overlay[at+3]=255;
    visible[pixel]=uint8_t(windowDepth[pixel]>0 && (sourceDepth[pixel]==0 || windowDepth[pixel]>sourceDepth[pixel]+.000001f) && (originalDepth[pixel]==0 || windowDepth[pixel]>=originalDepth[pixel]-.000001f));
    intrusion[pixel]=uint8_t(visible[pixel] && generatedDepth[pixel]>windowDepth[pixel]+.000001f);
   }
   for(size_t eye=0;eye<2;++eye){size_t samples=0,hits=0,outside=0,largest=0,structureHits=0;std::vector<uint8_t> mask(size_t(size)*size);
    const size_t first=eye*size_t(size)*size;
    for(size_t y=1;y+1<size;++y)for(size_t x=1;x+1<size;++x){const size_t local=y*size+x,pixel=first+local;
     const bool eroded=visible[pixel]&&visible[pixel-1]&&visible[pixel+1]&&visible[pixel-size]&&visible[pixel+size];samples+=eroded;
     const bool hit=eroded&&intrusion[pixel];hits+=hit;mask[local]=uint8_t(hit);
     const bool structuralHit=eroded&&structureDepth[pixel]>windowDepth[pixel]+.000001f && structureDepth[pixel]>=generatedDepth[pixel]-.000001f;structureHits+=structuralHit;
     const size_t at=pixel*4;if(structuralHit){structureMarked[at]=255;structureMarked[at+1]=96;structureMarked[at+2]=0;}outside+=overlay[at]==32 && overlay[at+1]>32;
     if(hit){marked[at]=255;marked[at+1]=96;marked[at+2]=0;}
    }
    for(size_t seed=0;seed<mask.size();++seed)if(mask[seed]){std::vector<size_t> queue{seed};mask[seed]=0;
     for(size_t i=0;i<queue.size();++i){const size_t q=queue[i];for(size_t n:{q-1,q+1,q-size,q+size})if(n<mask.size()&&mask[n]){mask[n]=0;queue.push_back(n);}}
     largest=std::max(largest,queue.size());
    }
    csv<<id<<','<<native<<','<<pose.name<<','<<eye<<','<<samples<<','<<hits<<','<<outside<<','<<largest<<','<<structureHits<<'\n';flagged+=hits>0;++eyeViews;
   }
   const auto save=[&](const char* suffix,const auto& image){gt2::WritePngRgba((dest/(std::string(pose.name)+suffix+".png")).string(),size,size*2,image);};
   save("-structure-intrusions",structureMarked);save("-normal",normal);save("-translucent",overlay);save("-window-intrusions",marked);csv.flush();
  }
  ++cars;std::cout<<id<<" aperture overlay complete\n"<<std::flush;
 }
 std::ofstream(out/"gate.json")<<"{\"cars\":"<<cars<<",\"eye_views\":"<<eyeViews<<",\"flagged_views\":"<<flagged<<",\"status\":\""<<(flagged?"requires_review":"no_detected_intrusion")<<"\",\"window_source\":\"original_apertures_before_replacement\",\"depth_convention\":\"reversed_same_cabin_band\"}\n";
 return flagged?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
