#pragma once
#include "gt2formats/gt_menu_images.h"
#include <string_view>
namespace gt2view {
inline std::string_view CockpitBadgeText(bool authored) {
    return authored?"ORIGINAL INTERIOR":"";
}
// Small platform-independent lettering, drawn after the car and menu artwork.
// Both disc menus mark authored interiors; generated cabins have no badge.
inline void AppendCockpitBadge(std::vector<gt2::MenuPrim>& out,bool authored,int x,int y) {
    if(!authored)return;
    static constexpr uint8_t letters[26][5]={
        {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
        {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
        {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
        {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
        {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
        {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
        {7,8,112,8,7},{97,81,73,69,67}};
    const auto text=CockpitBadgeText(authored);
    const auto tile=[&](int px,int py,int width,int height,uint32_t color){
        gt2::MenuPrim p;p.kind=gt2::MenuPrim::kTile;p.x[0]=int16_t(px);p.y[0]=int16_t(py);
        p.w=int16_t(width);p.h=int16_t(height);p.colour[0]=color;out.push_back(p);
    };
    tile(x-5,y-5,int(text.size())*6+9,24,0x201810);
    tile(x-5,y-5,2,24,0x70df90);
    for(char c:text){
        if(c>='A' && c<='Z')for(int col=0;col<5;++col)for(int row=0;row<7;++row)
            if((letters[c-'A'][col]>>row)&1)tile(x+col,y+row*2,1,2,0xe8f0f0);
        x+=6;
    }
}
}
