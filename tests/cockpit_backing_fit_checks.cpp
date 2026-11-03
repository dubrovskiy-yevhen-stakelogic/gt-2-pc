#include "gt2view/cockpit_fit.h"
#ifndef GT2_LEGACY_BACKING
#include "gt2view/cockpit_backing_fit.h"
#endif
#include <iostream>
#include <stdexcept>
using namespace gt2view;
int main(){try{
 CockpitFit f;f.valid=true;f.halfWidth=.8f;f.frontCowl={{{-.8f,.6f,-1},{.8f,.6f,-1}}};f.rearCowl={{{-.8f,.6f,1},{.8f,.6f,1}}};f.frontHeader={{{-.7f,1.1f,-1},{.7f,1.1f,-1}}};
 const std::array<float,3> a{.8f,.55f,-1},b{.8f,.55f,1},c{.7f,1.1f,1},d{.7f,1.1f,-1};f.liningApertures={{a,b,c},{a,c,d}};
 const std::vector<std::array<float,3>> original{{-.75f,.8f,-.8f},{.75f,.8f,-.8f},{.75f,.8f,.8f},{-.75f,.8f,.8f}};
#ifdef GT2_LEGACY_BACKING
 const auto result=original;
#else
 const auto result=FitBackingGlazingFootprint(f,original);
#endif
 bool low=false;
 for(const auto& q:result){if(q[1]>.80001f)throw std::runtime_error("backing raised");if(q[0]<0 && std::abs(q[1]-.8f)>1e-6f)throw std::runtime_error("opposite wall changed");if(q[0]>.749f){if(q[1]>.55001f)throw std::runtime_error("backing obstructs swept side glazing");low=true;}}
 if(!low)throw std::runtime_error("side wall missing");std::cout<<"Side-window footprint and unchanged opposite wall passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
