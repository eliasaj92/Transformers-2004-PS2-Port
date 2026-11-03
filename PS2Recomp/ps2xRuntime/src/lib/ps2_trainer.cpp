// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/ps2_trainer.h"
#include "runtime/ps2_settings.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_carstick_state.h"
#include "ps2_trainer_diagnostics.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sstream>

namespace ps2x::trainer {
namespace {
constexpr uint32_t supportedCrc = 0x74c982c0;
constexpr uint32_t healthResource = 0x716fb0;
PS2Runtime::RecompiledFunction subtractResource = nullptr;
PS2Runtime::RecompiledFunction updateEnergy = nullptr, setEnergy = nullptr;
std::atomic<bool> supported{false}, trainerProfile{false}, hub{false};
std::atomic<unsigned> pending{0};
bool energyEnabled=false;
float movementMultiplier=1;
uint32_t scaledContext=0;
uint32_t scaledActor=0,scaledCreature=0;
float originalScale=1, appliedScale=1;
float damageMultiplier=1;
std::unordered_map<uint32_t,PS2Runtime::RecompiledFunction> damageOriginals;
std::unordered_set<uint32_t> damageEntries;
struct DamageCall {uint32_t stack, caller, target, event;float original, applied;};
std::vector<DamageCall> damageCalls;
uint64_t blockedHealth=0,refilledEnergy=0,scaledDamage=0;
std::mutex messageMutex;
std::string message = "Normal save profile. Switch to Trainer and restart to enable hub unlocks.";
bool span(uint32_t address,uint32_t bytes) {return address>=0x100000 && bytes<=0x2000000 && address<=0x2000000-bytes;}
template<class T> T read(const uint8_t* ram,uint32_t address) {
    T value{};if(span(address,sizeof(T)))std::memcpy(&value,ram+address,sizeof(T));return value;
}
template<class T> void write(uint8_t* ram,uint32_t address,T value) {
    if(span(address,sizeof(T)))std::memcpy(ram+address,&value,sizeof(T));
}
uint32_t player(const uint8_t* ram) {
    const auto sample=ps2_carstick::snapshot();
    const auto controller=sample.controller;
    if(!span(controller,12) || read<uint32_t>(ram,controller)!=0x6b8000)return 0;
    const auto creature=read<uint32_t>(ram,controller+8);
    const auto actor=read<uint32_t>(ram,creature+0x54);
    const auto context=read<uint32_t>(ram,actor+0x7c);
    const auto binding=read<uint32_t>(ram,creature+0x38);
    return span(binding,4) && read<uint32_t>(ram,binding)==0 && actor==sample.actor &&
        read<uint32_t>(ram,context)==actor && read<uint32_t>(ram,context+4)==creature &&
        read<uint32_t>(ram,context+0xd4)==controller ? actor : 0;
}
bool playerWeapon(const uint8_t* ram,uint32_t weapon) {
    const auto actor=player(ram);if(!actor || !span(weapon,0x60))return false;
    // sub_002867d0 returns actor+0x34+slot*4, the four equipped Mini-Con abilities.
    for(unsigned slot=0;slot<4;++slot)if(read<uint32_t>(ram,actor+0x34+slot*4)==weapon)return true;
    return false;
}
void refill(uint8_t* ram,uint32_t weapon) {
    const int maximum=read<int>(ram,weapon+0x50);
    if(maximum<=0 || maximum>100000)return;
    write<int>(ram,weapon+0x3c,maximum);
    write<float>(ram,weapon+0x30,0);
    ++refilledEnergy;
}
void energyUpdate(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {
    if(!energyEnabled){updateEnergy(rdram,ctx,runtime);return;}
    const bool entry=ctx->pc==0x3ee400;
    const uint32_t weapon=GPR_U32(ctx,entry?4:17);
    const uint32_t stack=GPR_U32(ctx,29), callerStack=entry?stack:stack+0x40;
    const uint32_t caller=entry?GPR_U32(ctx,31):read<uint32_t>(rdram,stack+0x30);
    if(playerWeapon(rdram,weapon))refill(rdram,weapon);
    updateEnergy(rdram,ctx,runtime);
    if(ctx->pc==caller && GPR_U32(ctx,29)==callerStack && playerWeapon(rdram,weapon))refill(rdram,weapon);
}
void energySet(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {
    const auto weapon=GPR_U32(ctx,4);
    if(energyEnabled && playerWeapon(rdram,weapon)) {
        const int current=read<int>(rdram,weapon+0x3c), next=static_cast<int>(GPR_U32(ctx,5));
        if(next>=0 && next<current)SET_GPR_S32(ctx,5,current);
    }
    setEnergy(rdram,ctx,runtime);
}
bool inHub(const uint8_t* ram) {
    // Existing UI detector's owner -> manager -> active screen, with both backlinks.
    const auto owner=read<uint32_t>(ram,0x6d30f0-0x3b38);
    if(read<uint32_t>(ram,owner)!=0x6b7160)return false;
    const auto manager=read<uint32_t>(ram,owner+4), active=read<uint32_t>(ram,manager+0x3c);
    return read<uint32_t>(ram,active)==0x6bd8f0 && read<uint32_t>(ram,active+8)==manager &&
        read<uint32_t>(ram,active+0x54)==2;
}
void note(const std::string& text) {std::lock_guard<std::mutex> lock(messageMutex);message=text;}
void movement(uint8_t* ram) {
    const auto actor=player(ram), context=actor?read<uint32_t>(ram,actor+0x7c):0;
    if(scaledContext && scaledContext!=context) {
        // Restore a still-live former player before changing characters/contexts.
        // Recycled or game-modified storage is never written.
        if(read<uint32_t>(ram,scaledContext)==scaledActor &&
            read<uint32_t>(ram,scaledContext+4)==scaledCreature &&
            read<uint32_t>(ram,scaledActor+0x7c)==scaledContext &&
            read<uint32_t>(ram,scaledCreature+0x54)==scaledActor &&
            read<float>(ram,scaledContext+0x1fc)==appliedScale)
            write<float>(ram,scaledContext+0x1fc,originalScale);
        scaledContext=0;
    }
    if(!context || !span(context,0x200)){scaledContext=0;return;}
    const float current=read<float>(ram,context+0x1fc);
    if(!std::isfinite(current)||current<=0||current>100){scaledContext=0;return;}
    if(scaledContext!=context || current!=appliedScale)originalScale=current;
    if(movementMultiplier==1 && scaledContext!=context)return; // Off never writes ordinary game state.
    appliedScale=originalScale*movementMultiplier;
    if(appliedScale!=current)write<float>(ram,context+0x1fc,appliedScale);
    scaledContext=movementMultiplier==1?0:context;
    scaledActor=actor;scaledCreature=read<uint32_t>(ram,context+4);
}
void health(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    // SLUS-20668: godModePlayer's caller resolves the health object through
    // 0x34f2d0 -> 0x716ef0 and 0x350a70 -> +0xc0. This leaf subtracts f12.
    if (GPR_U32(ctx,4)==healthResource && std::isfinite(ctx->f[12]) && ctx->f[12]>0 &&
        settings::getBool("trainer.infinite_health")) {ctx->f[12]=0;++blockedHealth;}
    subtractResource(rdram,ctx,runtime);
}
void damage(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    const auto original=damageOriginals.at(ctx->pc);
    if(damageMultiplier>1 && damageEntries.count(ctx->pc) && damageCalls.size()<64) {
        const auto actor=player(ram);
        const auto creature=actor?read<uint32_t>(ram,read<uint32_t>(ram,actor+0x7c)+4):0;
        const auto target=GPR_U32(ctx,4), event=GPR_U32(ctx,5);
        const float amount=read<float>(ram,event+0x54);
        // Hit builders 0x505580/0x505740/0x505ab0 write source creature at +0x30,
        // recipient at +0x38 and damage at +0x54. Require all three identities.
        // Derived recipients can copy the event and call a base implementation;
        // keep one multiplier for that entire guest call, including suspended calls.
        const bool nested=std::any_of(damageCalls.begin(),damageCalls.end(),
            [target](const auto& call){return call.target==target;});
        if(creature && target!=creature && span(event,0x60) &&
            read<uint32_t>(ram,event+0x30)==creature && read<uint32_t>(ram,event+0x38)==target &&
            !nested && std::isfinite(amount) && amount>0 && std::isfinite(amount*damageMultiplier)) {
            damageCalls.push_back({GPR_U32(ctx,29),GPR_U32(ctx,31),target,event,amount,amount*damageMultiplier});
            write<float>(ram,event+0x54,amount*damageMultiplier);
            ++scaledDamage;
        }
    }
    original(ram,ctx,runtime);
    for(size_t i=damageCalls.size();i-->0;) {
        const auto call=damageCalls[i];
        if(ctx->pc!=call.caller || GPR_U32(ctx,29)!=call.stack)continue;
        // Restore reusable caller-owned event storage; never leave a permanent
        // change in weapon attributes or multiply the next target a second time.
        if(read<float>(ram,call.event+0x54)==call.applied)write<float>(ram,call.event+0x54,call.original);
        damageCalls.erase(damageCalls.begin()+i);
    }
}
}
void install(PS2Runtime& runtime, uint32_t crc) {
    if (crc != supportedCrc || !runtime.hasFunction(0x350650)) return;
    subtractResource=runtime.lookupFunction(0x350650);
    runtime.replaceFunction(0x350650,&health);
    updateEnergy=runtime.hasFunction(0x3ee400)?runtime.lookupFunction(0x3ee400):nullptr;
    setEnergy=runtime.hasFunction(0x3ef580)?runtime.lookupFunction(0x3ef580):nullptr;
    if(updateEnergy && setEnergy) {
        // A resumed guest call must reach the wrapper too; the runtime table contains
        // aliases at continuation PCs as well as the function entry.
        for(uint32_t pc=0x3ee400;pc<0x3ee700;pc+=4)
            if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==updateEnergy)runtime.replaceFunction(pc,&energyUpdate);
        runtime.replaceFunction(0x3ef580,&energySet);
        settings::declareWired("trainer-energy");
    }
    // The +0x18 virtual methods of SLUS-20668 taTakeDamageBase descendants.
    // Player damage is deliberately absent. Shared methods are installed once.
    constexpr uint32_t recipients[]={0x2a6300,0x2aabf0,0x33fcb0,0x357ff0,0x3e29a0,
        0x404a40,0x49d790,0x49e840,0x4a0100,0x4b2690,0x4d4870,0x4d8a40,
        0x4dbeb0,0x543760,0x56cf70,0x5bca40};
    std::unordered_set<PS2Runtime::RecompiledFunction> methods;
    for(const auto entry:recipients)if(runtime.hasFunction(entry)) {
        methods.insert(runtime.lookupFunction(entry));damageEntries.insert(entry);
    }
    if(damageEntries.size()==std::size(recipients)) {
        for(uint32_t i=0;i<g_ps2RecompiledFunctionTableSlotCount;++i) {
            const auto function=g_ps2RecompiledFunctionTable[i];
            if(!function || !methods.count(function))continue;
            const uint32_t pc=g_ps2RecompiledFunctionTableBase+4*i;
            damageOriginals.emplace(pc,function);
            runtime.replaceFunction(pc,&damage);
        }
        settings::declareWired("trainer-damage");
    }
    supported.store(true);
    settings::declareWired("trainer-health");
    settings::declareWired("trainer-movement");
    std::fprintf(stderr,"[trainer] SLUS-20668 CRC %08x: player health hook installed; defaults off\n",crc);
}
std::filesystem::path selectSaveRoot(const std::filesystem::path& normalRoot) {
    namespace fs=std::filesystem;
    trainerProfile.store(false);
    if (settings::get("trainer.profile")!="trainer") return normalRoot;
    if (!supported.load()) throw std::runtime_error("Trainer profile requires the supported SLUS-20668 executable");
    const auto normal=fs::absolute(normalRoot).lexically_normal();
    const auto profile=normal.parent_path()/"profiles"/"trainer";
    const auto card=profile/"mc0";
    const auto expected=fs::weakly_canonical(normal.parent_path())/"profiles"/"trainer"/"mc0";
    if(fs::weakly_canonical(card)!=expected || fs::is_symlink(profile) || fs::is_symlink(card))
        throw std::runtime_error("Trainer card must be a separate directory");
    if(fs::exists(normal) && !fs::is_directory(normal))throw std::runtime_error("Normal memory card is not a directory");
    if(fs::exists(profile) && !fs::exists(card))throw std::runtime_error("Incomplete Trainer profile; retain it and choose a fresh save root");
    if (!fs::exists(card)) {
        fs::create_directories(profile.parent_path());
        const auto token=std::chrono::steady_clock::now().time_since_epoch().count();
        const auto staging=profile.parent_path()/("trainer-pending-"+std::to_string(token));
        fs::create_directories(staging/"mc0");
        if (fs::exists(normal)) for (const auto& item:fs::recursive_directory_iterator(normal)) {
            if (item.is_symlink()) throw std::runtime_error("Cannot clone a memory card containing symlinks");
            const auto destination=staging/"mc0"/item.path().lexically_relative(normal);
            if (item.is_directory()) fs::create_directories(destination);
            else if (item.is_regular_file()) fs::copy_file(item.path(),destination);
        }
        // Rename only after the entire copy succeeds. The normal card is never written.
        fs::rename(staging,profile);
    }
    if (fs::is_symlink(profile) || fs::is_symlink(card) ||
        fs::weakly_canonical(card)!=expected ||
        (fs::exists(normal) && fs::equivalent(card,normal)))
        throw std::runtime_error("Trainer card must be a separate directory");
    trainerProfile.store(true);
    note("Trainer save profile active. Return to the hub's main ring to unlock equipment or levels.");
    std::fprintf(stderr,"[trainer] active card: %s\n",card.string().c_str());
    return card;
}
void service(uint8_t* rdram, R5900Context*, PS2Runtime*) {
    if(!supported.load())return;
    energyEnabled=settings::getBool("trainer.unlimited_energy");
    damageMultiplier=static_cast<float>(settings::getFloat("trainer.damage_multiplier"));
    if(damageMultiplier!=1 && damageMultiplier!=2 && damageMultiplier!=4 && damageMultiplier!=8)damageMultiplier=1;
    movementMultiplier=static_cast<float>(settings::getFloat("trainer.movement_multiplier"));
    if(movementMultiplier!=1 && movementMultiplier!=1.25f && movementMultiplier!=1.5f && movementMultiplier!=2)movementMultiplier=1;
    movement(rdram);
    hub.store(inHub(rdram));
    const auto actions=pending.exchange(0);
    if(!actions)return;
    if(!canUnlock()){note("Unlock cancelled: return to the hub in the Trainer profile.");return;}
    constexpr uint32_t progress=0x7170e0;
    if(actions&static_cast<unsigned>(Unlock::MiniCons)) {
        // Forty valid abilities: starter bit plus the distinct grants in the original
        // game's seven episode grant routines (0x5ca350..0x5cafb0). Preserve other flags.
        constexpr uint64_t miniCons=0x0000f7ff7dbf4dffull;
        write<uint64_t>(rdram,progress+0x2e0,read<uint64_t>(rdram,progress+0x2e0)|miniCons);
    }
    if(actions&static_cast<unsigned>(Unlock::Levels)) {
        // sub_00351c10 tests availability bit 0; 0x351aa0 tests completion bit 1.
        // Main campaign indices 0..7, leaving the ninth internal episode untouched.
        for(unsigned level=0;level<8;++level) {
            const uint32_t row=progress+0x34+level*0x4c;
            write<uint32_t>(rdram,row,read<uint32_t>(rdram,row)|1u);
            write<uint32_t>(rdram,row+4,2u); // first warp gate available (0x3516b0)
        }
    }
    note("Unlock applied. Reopen equipment or mission selection, then save in the game. Completion progress is unchanged.");
    std::fprintf(stderr,"[trainer] hub unlock actions=%u applied to Trainer profile\n",actions);
}
std::string status() {std::lock_guard<std::mutex> lock(messageMutex);return message;}
bool canUnlock() {return supported.load() && trainerProfile.load() && hub.load();}
bool requestUnlock(Unlock action) {
    if (!canUnlock()) return false;
    pending.fetch_or(static_cast<unsigned>(action)); return true;
}
std::string diagnostics(const uint8_t* ram) {
    const auto actor=player(ram),context=actor?read<uint32_t>(ram,actor+0x7c):0;
    unsigned available=0,completed=0;
    for(unsigned i=0;i<8;++i){const auto flags=read<uint32_t>(ram,0x717114+i*0x4c);available|=(flags&1)<<i;completed|=((flags>>1)&1)<<i;}
    std::ostringstream out;
    out<<"{\"supported\":"<<supported.load()<<",\"trainer_profile\":"<<trainerProfile.load()
       <<",\"hub\":"<<hub.load()<<",\"actor\":"<<actor<<",\"context\":"<<context
       <<",\"actor_vtable\":"<<(actor?read<uint32_t>(ram,actor):0)
       <<",\"mode\":\""<<ps2_carstick::name(ps2_carstick::snapshot().mode)<<'"'
       <<",\"health\":"<<read<float>(ram,healthResource+4)<<",\"health_max\":"<<read<float>(ram,healthResource+12)
       <<",\"movement_scale\":"<<(context?read<float>(ram,context+0x1fc):0)
       <<",\"health_blocked\":"<<blockedHealth<<",\"energy_refills\":"<<refilledEnergy<<",\"damage_scaled\":"<<scaledDamage
       <<",\"minicons\":"<<read<uint64_t>(ram,0x7173c0)<<",\"levels_available\":"<<available<<",\"levels_completed\":"<<completed
       <<",\"weapons\":[";
    for(unsigned i=0;i<4;++i){const auto w=actor?read<uint32_t>(ram,actor+0x34+4*i):0;if(i)out<<',';
        out<<"{\"address\":"<<w<<",\"energy\":"<<(w?read<int>(ram,w+0x3c):0)<<",\"maximum\":"<<(w?read<int>(ram,w+0x50):0)<<'}';}
    out<<"]}";return out.str();
}
}
