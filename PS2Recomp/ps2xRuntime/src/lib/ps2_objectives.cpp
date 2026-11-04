// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_objectives.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_carstick_state.h"
#include "runtime/ps2_settings.h"
#include "raylib.h"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace ps2_objectives {
namespace {
using Clock=std::chrono::steady_clock;
PS2Runtime::RecompiledFunction update=nullptr;
std::unordered_map<uint32_t,Clock::time_point> triggers; // EE thread only
bool supported=false;
struct View {bool valid=false;float angle=0,distance=0;const char* label="";Clock::time_point time{};};
std::mutex viewMutex;
View view;
bool span(uint32_t p,uint32_t n){return p>=0x100000 && n<=0x2000000 && p<=0x2000000-n;}
template<class T>T read(const uint8_t* ram,uint32_t p){T v{};if(span(p,sizeof(v)))std::memcpy(&v,ram+p,sizeof(v));return v;}
// Curated Amazon guidance chain. These are event identifiers, not extracted assets
// or hard-coded world coordinates. Other episodes deliberately produce no marker.
struct Stage {const char* event;const char* label;};
constexpr Stage stages[]={
    {"HQ_RobotTraining","Training area"}, {"HQ_CombatTraining","Combat training"},
    {"HQ_ToDropship","Dropship route"}, {"HQ_ToRuins","Ruins route"},
    {"HQ_ToWaterfall","Waterfall route"}, {"HQ_FollowRoad","Mountain road"},
    {"HQ_ToMachuPicchu","Mountain ruins"}
};
bool eventIs(const uint8_t* ram,uint32_t object,const char* wanted){
    // Trigger +a0 -> interned string: tag/capacity +4, length +8, heap chars +c.
    const auto text=read<uint32_t>(ram,object+0xa0);
    if(!span(text,16) || !(read<uint32_t>(ram,text+4)&1))return false;
    const auto length=read<uint32_t>(ram,text+8),chars=read<uint32_t>(ram,text+12);
    return length==std::strlen(wanted) && span(chars,length+1) &&
        std::memcmp(ram+chars,wanted,length+1)==0;
}
void hook(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime){
    if(ctx->pc==0x2fa240){const auto object=GPR_U32(ctx,4);
        if(span(object,0xb0) && read<uint32_t>(ram,object)==0x6b6910)
            triggers[object]=Clock::now();
    }
    update(ram,ctx,runtime);
}
}
void install(PS2Runtime& runtime,uint32_t crc){
    if(crc!=0x74c982c0 || !runtime.hasFunction(0x2fa240))return;
    update=runtime.lookupFunction(0x2fa240);
    runtime.replaceFunction(0x2fa240,&hook);
    supported=true;
    ps2x::settings::declareWired("objective-guidance");
}
void sample(const uint8_t* ram){
    View next;next.time=Clock::now();
    if(supported){
        for(auto it=triggers.begin();it!=triggers.end();){
            if(next.time-it->second>std::chrono::seconds(2) || read<uint32_t>(ram,it->first)!=0x6b6910)it=triggers.erase(it);
            else ++it;
        }
        const auto player=ps2_carstick::snapshot();
        const auto actor=player.actor,controller=player.controller;
        const auto context=read<uint32_t>(ram,actor+0x7c),creature=read<uint32_t>(ram,controller+8);
        const bool valid=span(actor,0x100) && span(context,0x200) &&
            read<uint32_t>(ram,controller)==0x6b8000 && read<uint32_t>(ram,creature+0x54)==actor &&
            read<uint32_t>(ram,context)==actor && read<uint32_t>(ram,context+4)==creature &&
            read<uint32_t>(ram,context+0xd4)==controller;
        if(ps2x::settings::getBool("game.objective_guidance") && valid &&
           read<int>(ram,0x716ef0)==0 && read<int>(ram,actor+0xf8)!=0x16 && read<int>(ram,actor+0xf8)!=0x17 &&
           (player.mode==ps2_carstick::Mode::Robot || player.mode==ps2_carstick::Mode::Vehicle)){
            for(const auto& stage:stages){
                uint32_t candidate=0;
                for(const auto& [object,seen]:triggers){
                    if(!eventIs(ram,object,stage.event) || read<int>(ram,object+0x78)<=0)continue;
                    const auto previous=read<uint32_t>(ram,object+0x80);
                    if(previous && (read<uint32_t>(ram,previous)!=0x6b6910 || read<int>(ram,previous+0x78)>0))continue;
                    if(candidate){candidate=0;break;} // Ambiguous state: don't invent a target.
                    candidate=object;
                }
                if(!candidate)continue;
                const float dx=read<float>(ram,candidate+0x40)-read<float>(ram,context+0xf0);
                const float dy=read<float>(ram,candidate+0x44)-read<float>(ram,context+0xf4);
                const float dz=read<float>(ram,candidate+0x48)-read<float>(ram,context+0xf8);
                next.distance=std::sqrt(dx*dx+dy*dy+dz*dz);
                next.angle=std::atan2(dy,dx)-read<float>(ram,context+0x90);
                next.valid=std::isfinite(next.distance) && next.distance<100000 && std::isfinite(next.angle);
                next.label=stage.label;
                break;
            }
        }
    }
    static const char* lastLabel="";
    const char* label=next.valid?next.label:"";
    if(std::strcmp(lastLabel,label)!=0){
        std::fprintf(stderr,"[objective] %s distance=%.1f\n",*label?label:"hidden",next.distance);
        lastLabel=label;
    }
    std::lock_guard<std::mutex> lock(viewMutex);view=next;
}
void draw(float x,float y,float width,float height){
    View current;{std::lock_guard<std::mutex> lock(viewMutex);current=view;}
    if(!current.valid || Clock::now()-current.time>std::chrono::milliseconds(300) || width<=0 || height<=0)return;
    const float scale=std::clamp(height/1080.f,0.6f,2.f),cx=x+width/2,cy=y+height-76*scale;
    const Vector2 forward{-std::sin(current.angle),-std::cos(current.angle)};
    const Vector2 right{-forward.y,forward.x};
    auto point=[&](float f,float r){return Vector2{cx+(forward.x*f+right.x*r)*scale,cy+(forward.y*f+right.y*r)*scale};};
    DrawRectangleRounded({cx-190*scale,cy-34*scale,380*scale,88*scale},0.25f,6,Color{10,20,26,210});
    DrawCircleLines(int(cx),int(cy),24*scale,Color{65,160,190,255});
    // Arrow is relative to Autobot heading; camera-only movement does not rotate it.
    const auto tip=point(21,0),left=point(-12,-10),rightPoint=point(-12,10);
    DrawTriangle(tip,left,rightPoint,Color{95,235,255,255});
    const char* text=TextFormat("%s  %.0f",current.label,current.distance);
    const int font=int(18*scale);
    DrawText(text,int(cx-MeasureText(text,font)/2),int(cy+27*scale),font,RAYWHITE);
}
}
