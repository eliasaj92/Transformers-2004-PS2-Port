// Synthetic fixtures only. This compiles and exercises the production trainer itself.
#include "ps2_runtime.h"
#include "../../PS2Recomp/ps2xRuntime/src/lib/ps2_carstick_state.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_trainer.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
const uint32_t g_ps2RecompiledFunctionTableBase=0x100000;
const uint32_t g_ps2RecompiledFunctionTableSlotCount=0x500000/4;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount]{};
bool PS2Runtime::hasFunction(uint32_t p)const{return lookupFunction(p)!=nullptr;}
PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t p)const{
    return p>=0x100000 && p<0x600000 && !(p&3)?g_ps2RecompiledFunctionTable[(p-0x100000)/4]:nullptr;
}
bool PS2Runtime::replaceFunction(uint32_t p,RecompiledFunction f){g_ps2RecompiledFunctionTable[(p-0x100000)/4]=f;return true;}
template<class T>T get(const uint8_t* r,uint32_t a){T v;std::memcpy(&v,r+a,sizeof(v));return v;}
template<class T>void put(uint8_t* r,uint32_t a,T v){std::memcpy(r+a,&v,sizeof(v));}
void require(bool condition,const char* what){if(!condition)throw std::runtime_error(what);}
void resource(uint8_t* r,R5900Context* c,PS2Runtime*){
    const auto a=c->r[4];put<float>(r,a+4,std::max(0.0f,get<float>(r,a+4)-c->f[12]));c->pc=c->r[31];
}
void energy(uint8_t* r,R5900Context* c,PS2Runtime*){--*reinterpret_cast<int*>(r+c->r[4]+0x3c);c->pc=c->r[31];}
void energySet(uint8_t* r,R5900Context* c,PS2Runtime*){put<int>(r,c->r[4]+0x3c,int(c->r[5]));c->pc=c->r[31];}
std::vector<float> damageObserved;
bool nested=false,suspend=false;
uint32_t suspendedEvent=0,suspendedReturn=0;
void hit(uint8_t* r,R5900Context* c,PS2Runtime* rt){
    if(c->pc==0x2aabf4){damageObserved.push_back(get<float>(r,suspendedEvent+0x54));c->r[29]+=0x80;c->pc=suspendedReturn;return;}
    const auto e=c->r[5];damageObserved.push_back(get<float>(r,e+0x54));
    if(suspend){suspend=false;suspendedEvent=e;suspendedReturn=c->r[31];c->r[29]-=0x80;c->pc=0x2aabf4;return;}
    if(nested){nested=false;auto inner=*c;inner.r[29]-=0x100;inner.r[31]=0x2aac00;inner.r[5]=0xf00100;
        std::memcpy(r+inner.r[5],r+e,0x60);inner.pc=0x2a6300;rt->lookupFunction(inner.pc)(r,&inner,rt);}
    c->pc=c->r[31];
}
int main(int argc,char** argv){try{
    require(argc==2,"private fixture directory required");
    std::vector<uint8_t> memory(0x2000000);auto* ram=memory.data();PS2Runtime rt;R5900Context c;
    namespace s=ps2x::settings;namespace t=ps2x::trainer;namespace fs=std::filesystem;
    rt.replaceFunction(0x350650,resource);rt.replaceFunction(0x3ee400,energy);rt.replaceFunction(0x3ef580,energySet);
    for(auto a:{0x2a6300u,0x2aabf0u,0x33fcb0u,0x357ff0u,0x3e29a0u,0x404a40u,0x49d790u,0x49e840u,
        0x4a0100u,0x4b2690u,0x4d4870u,0x4d8a40u,0x4dbeb0u,0x543760u,0x56cf70u,0x5bca40u,0x2aabf4u})rt.replaceFunction(a,hit);
    t::install(rt,0);require(rt.lookupFunction(0x350650)==resource,"wrong build modified");
    t::install(rt,0x74c982c0);require(s::wired.count("trainer-damage"),"damage hook unavailable");
    auto call=[&](uint32_t pc,uint32_t a,uint32_t b=0,float f=0){c={};c.pc=pc;c.r[4]=a;c.r[5]=b;c.r[29]=0x1ff0000;c.r[31]=0x123456;c.f[12]=f;rt.lookupFunction(pc)(ram,&c,&rt);};
    put<float>(ram,0x716fb4,100);call(0x350650,0x716fb0,0,20);require(get<float>(ram,0x716fb4)==80,"health off changed behavior");
    s::values["trainer.infinite_health"]="on";call(0x350650,0x716fb0,0,20);require(get<float>(ram,0x716fb4)==80,"player not protected");
    put<float>(ram,0x900004,100);call(0x350650,0x900000,0,20);require(get<float>(ram,0x900004)==80,"NPC health protected");
    s::values["trainer.infinite_health"]="off";call(0x350650,0x716fb0,0,20);require(get<float>(ram,0x716fb4)==60,"health toggle not restored");
    constexpr uint32_t ctl=0x800000,creature=0x801000,actor=0x802000,context=0x803000,weapon=0x804000,binding=0x805000;
    ps2_carstick::latest.controller=ctl;ps2_carstick::latest.actor=actor;
    put<uint32_t>(ram,ctl,0x6b8000);put<uint32_t>(ram,ctl+8,creature);
    put<uint32_t>(ram,creature+0x54,actor);put<uint32_t>(ram,creature+0x38,binding);put<uint32_t>(ram,actor+0x7c,context);
    put<uint32_t>(ram,context,actor);put<uint32_t>(ram,context+4,creature);put<uint32_t>(ram,context+0xd4,ctl);
    put<uint32_t>(ram,actor+0x34,weapon);put<int>(ram,weapon+0x50,100);put<int>(ram,weapon+0x3c,25);put<float>(ram,context+0x1fc,3);
    s::values["trainer.unlimited_energy"]="on";s::values["trainer.movement_multiplier"]="1.5";t::service(ram,&c,&rt);
    call(0x3ee400,weapon);require(get<int>(ram,weapon+0x3c)==100,"energy refill failed");
    call(0x3ef580,weapon,5);require(get<int>(ram,weapon+0x3c)==100,"energy setter drained player");
    put<int>(ram,0x90003c,25);call(0x3ee400,0x900000);require(get<int>(ram,0x90003c)==24,"NPC energy modified");
    require(get<float>(ram,context+0x1fc)==4.5f,"movement not scaled");t::service(ram,&c,&rt);require(get<float>(ram,context+0x1fc)==4.5f,"movement compounded");
    s::values["trainer.unlimited_energy"]="off";s::values["trainer.movement_multiplier"]="1";t::service(ram,&c,&rt);
    require(get<float>(ram,context+0x1fc)==3,"movement not restored");call(0x3ee400,weapon);require(get<int>(ram,weapon+0x3c)==99,"energy toggle not restored");
    s::values["trainer.movement_multiplier"]="2";t::service(ram,&c,&rt);
    constexpr uint32_t ctl2=0x810000,creature2=0x811000,actor2=0x812000,context2=0x813000;
    put<uint32_t>(ram,ctl2,0x6b8000);put<uint32_t>(ram,ctl2+8,creature2);put<uint32_t>(ram,creature2+0x54,actor2);
    put<uint32_t>(ram,creature2+0x38,binding);put<uint32_t>(ram,actor2+0x7c,context2);put<uint32_t>(ram,context2,actor2);
    put<uint32_t>(ram,context2+4,creature2);put<uint32_t>(ram,context2+0xd4,ctl2);put<float>(ram,context2+0x1fc,5);
    ps2_carstick::latest.controller=ctl2;ps2_carstick::latest.actor=actor2;t::service(ram,&c,&rt);
    require(get<float>(ram,context+0x1fc)==3 && get<float>(ram,context2+0x1fc)==10,"character switch left compounded movement");
    s::values["trainer.movement_multiplier"]="1";t::service(ram,&c,&rt);require(get<float>(ram,context2+0x1fc)==5,"second character movement not restored");
    ps2_carstick::latest.controller=ctl;ps2_carstick::latest.actor=actor;t::service(ram,&c,&rt);
    constexpr uint32_t event=0xf00000,target=0x900000;
    put<uint32_t>(ram,event+0x30,creature);put<uint32_t>(ram,event+0x38,target);put<float>(ram,event+0x54,10);
    s::values["trainer.damage_multiplier"]="4";t::service(ram,&c,&rt);nested=true;call(0x2aabf0,target,event);
    require(damageObserved==std::vector<float>({40,40}),"nested damage multiplied twice");require(get<float>(ram,event+0x54)==10,"event not restored");
    damageObserved.clear();suspend=true;call(0x2aabf0,target,event);require(get<float>(ram,event+0x54)==40,"suspended damage lost");
    rt.lookupFunction(c.pc)(ram,&c,&rt);require(damageObserved==std::vector<float>({40,40}),"resume damage incorrect");require(get<float>(ram,event+0x54)==10,"resumed event not restored");
    damageObserved.clear();put<uint32_t>(ram,event+0x30,0x901000);call(0x2aabf0,target,event);require(damageObserved.back()==10,"NPC attack multiplied");
    put<uint32_t>(ram,event+0x30,creature);put<uint32_t>(ram,event+0x38,creature);call(0x2aabf0,creature,event);require(damageObserved.back()==10,"self damage multiplied");
    put<uint32_t>(ram,event+0x38,target);s::values["trainer.damage_multiplier"]="1";t::service(ram,&c,&rt);call(0x2aabf0,target,event);require(damageObserved.back()==10,"damage toggle not restored");
    const fs::path normal=fs::path(argv[1])/"saves"/"mc0";fs::create_directories(normal);std::ofstream(normal/"save")<<"normal";
    s::values["trainer.profile"]="normal";require(t::selectSaveRoot(normal)==normal,"normal profile redirected");
    require(!t::requestUnlock(t::Unlock::MiniCons),"normal profile unlock permitted");
    s::values["trainer.profile"]="trainer";const auto card=t::selectSaveRoot(normal);require(card!=normal && fs::exists(card/"save"),"trainer save not cloned");
    std::ofstream(card/"save")<<"trainer";std::string text;std::ifstream(normal/"save")>>text;require(text=="normal","normal save modified");
    require(t::selectSaveRoot(normal)==card,"trainer profile changed");std::ifstream(card/"save")>>text;require(text=="trainer","trainer card recloned over progress");
    constexpr uint32_t owner=0xa00000,manager=0xa01000,screen=0xa02000,progress=0x7170e0;
    put<uint32_t>(ram,0x6cf5b8,owner);put<uint32_t>(ram,owner,0x6b7160);put<uint32_t>(ram,owner+4,manager);
    put<uint32_t>(ram,manager+0x3c,screen);put<uint32_t>(ram,screen,0x6bd8f0);put<uint32_t>(ram,screen+8,manager);put<uint32_t>(ram,screen+0x54,2);
    for(unsigned i=0;i<9;++i)put<uint32_t>(ram,progress+0x34+i*0x4c,i==2?6u:4u);
    t::service(ram,&c,&rt);require(t::canUnlock(),"hub not recognized");t::requestUnlock(t::Unlock::Levels);t::requestUnlock(t::Unlock::MiniCons);t::service(ram,&c,&rt);
    require(get<uint64_t>(ram,progress+0x2e0)==0x0000f7ff7dbf4dffull,"incorrect equipment mask");
    for(unsigned i=0;i<8;++i){require(get<uint32_t>(ram,progress+0x34+i*0x4c)==(i==2?7u:5u),"completion bits changed");require(get<uint32_t>(ram,progress+0x38+i*0x4c)==2,"first gate locked");}
    require(get<uint32_t>(ram,progress+0x34+8*0x4c)==4,"internal episode changed");
    put<uint64_t>(ram,progress+0x2e0,0);t::requestUnlock(t::Unlock::MiniCons);put<uint32_t>(ram,screen+0x54,1);t::service(ram,&c,&rt);
    require(get<uint64_t>(ram,progress+0x2e0)==0,"queued unlock escaped hub restriction");
    std::cout<<"PASS trainer: build guard, ownership, toggles, nested/resumed damage, profile isolation, hub unlocks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
