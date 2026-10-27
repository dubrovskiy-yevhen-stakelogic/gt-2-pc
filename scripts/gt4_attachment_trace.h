#pragma once
#include "gt4formats/mdls.h"

// Follow the same selected command path as drawList, retaining the host wheel
// transform that its static matrix walker cannot evaluate. CORE.GT4 US 1.01,
// CarModel::Callback 00453698, wheel placement 00454110; see the owned
// gt4-pc/docs/formats/car4.md. Never infer attachments from vertex bounds.
struct CarAttachment { uint32_t shape; int wheel=-1; bool spinning=false; };
inline std::vector<CarAttachment> CarAttachments(gt4fmt::Bytes bytes,const gt4fmt::ModelSet& ms,const int* callbacks) {
    using namespace gt4fmt;
    std::vector<CarAttachment> out;
    struct State { int wheel=-1;bool spinning=false; } state;
    std::vector<State> stack;
    auto walk=[&](auto&& self,int model,int depth)->void {
        if(depth>=8)throw FormatError("attachment model recursion");
        const auto offset=ms.models.at(size_t(model)).cmdOffset;
        if(!offset)return;
        size_t p=ms.base+offset;
        for(int step=0;step<100000;++step) {
            const auto op=rd8(bytes,p);if(!op)return;
            size_t next=p+commandLength(bytes,p);
            if(op==2 || op==3)self(self,op==2?rd8(bytes,p+1):rd16(bytes,p+1),depth+1);
            else if(op==4 || op==5 || op==0x35)out.push_back({op==4?uint32_t(rd8(bytes,p+1)):uint32_t(rd16(bytes,p+1)),state.wheel,state.spinning});
            else if(op==0x0c)stack.push_back(state);
            else if(op==0x0d){if(stack.empty())throw FormatError("attachment stack underflow");state=stack.back();stack.pop_back();}
            else if(op==6 || op==0x4d) {
                const auto id=rd16(bytes,p+1);const auto n=rd8(bytes,p+3);
                if(op==6 && id>=0x0b && id<=0x0e){state.wheel=id-0x0b;state.spinning=false;}
                if(op==6 && id>=0x0f && id<=0x12){state.wheel=id-0x0f;state.spinning=true;}
                const int value=op==6 && id<0x30?callbacks[id]:0;
                if(value>=0 && value<n){const auto q=p+4+2*size_t(value);next=size_t(int64_t(q)+int16_t(rd16(bytes,q)));}
            } else if(op==7){const auto q=p+0x12;next=size_t(int64_t(q)+int16_t(rd16(bytes,q)));}
            else if(op==8)next=size_t(int64_t(p)+1+int8_t(rd8(bytes,p+1)));
            else if(op==9)next=size_t(int64_t(p)+1+int16_t(rd16(bytes,p+1)));
            p=next;
        }
        throw FormatError("attachment command walk does not terminate");
    };
    walk(walk,0,0);return out;
}
