// SPDX-License-Identifier: GPL-2.0-only
// Serialized presentation copies. None of these values are written to game state.
#include "Native.h"
extern "C" {
#include "doomstat.h"
#include "p_local.h"
#include "r_state.h"
#include "i_system.h"
extern int gametic;
extern boolean menuactive;
extern int native_mouseturn;
int G_NativePendingTurn(void);
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cstdio>
namespace {
struct Actor {
    uint64_t generation=0;
    int32_t x=0,y=0,z=0,health=0;
    uint32_t angle=0;
};
struct Plane { int32_t floor,ceiling; };
struct Camera {
    const mobj_t *actor=nullptr;
    int32_t z=0;
    int weapon=0,life=0;
    std::array<pspdef_t,NUMPSPRITES> sprites{};
};
struct Snapshot {
    bool valid=false;
    int time=0,episode=0,map=0,clock=0;
    std::unordered_map<const mobj_t *,Actor> actors;
    std::vector<Plane> planes;
    std::array<Camera,MAXPLAYERS> cameras{};
};
Snapshot older,newer;
struct Boundary { int command=-1,clock=0; };
std::array<Boundary,BACKUPTICS> boundaries{};
std::unordered_map<const mobj_t *,uint64_t> generations;
std::unordered_map<const mobj_t *,mobj_t> renderActors;
std::vector<sector_t> renderSectors;
uint64_t nextGeneration=0;
double fraction=1;
bool renderReady=false;
bool enabled() { return Native_Enhanced(); }
int32_t blend(int32_t a,int32_t b) {
    return int32_t(int64_t(a)+std::llround((int64_t(b)-a)*fraction));
}
uint32_t blendAngle(uint32_t a,uint32_t b) {
    int64_t delta=int32_t(b-a);
    return a+uint32_t(std::llround(delta*fraction));
}
bool continuous(const Actor &a,const Actor &b) {
    return a.generation==b.generation && (a.health>0)==(b.health>0) &&
        std::llabs(int64_t(b.x)-a.x)<=128LL*FRACUNIT &&
        std::llabs(int64_t(b.y)-a.y)<=128LL*FRACUNIT &&
        std::llabs(int64_t(b.z)-a.z)<=64LL*FRACUNIT;
}
}
void Native_CommandBoundary(int commandTic,int clockTic) {
    boundaries[unsigned(commandTic)%BACKUPTICS]={commandTic,clockTic};
}
void Native_RenderReset(void) {
    Native_GoreReset();
    older.valid=newer.valid=false;
    older.actors.clear(); newer.actors.clear(); generations.clear();
    renderActors.clear(); renderSectors.clear(); renderReady=false;
}
void Native_RenderDiscontinuity(void *actor) {
    generations[static_cast<mobj_t *>(actor)]=++nextGeneration;
}
void Native_RenderDespawn(void *actor) { generations.erase(static_cast<mobj_t *>(actor)); }
void Native_RenderSnapshot(void) {
    if(!enabled()) return;
    if(gamestate!=GS_LEVEL || !sectors) { older.valid=newer.valid=false; return; }
    std::swap(older,newer);
    const Boundary &boundary=boundaries[unsigned(gametic)%BACKUPTICS];
    newer.valid=true; newer.time=leveltime; newer.episode=gameepisode; newer.map=gamemap;
    newer.clock=boundary.command==gametic?boundary.clock:Native_Time();
    newer.actors.clear(); newer.planes.resize(numsectors);
    for(thinker_t *t=thinkercap.next;t!=&thinkercap;t=t->next) if(t->function.acp1==(actionf_p1)P_MobjThinker) {
        auto *m=reinterpret_cast<mobj_t *>(t);
        auto found=generations.find(m);
        uint64_t generation=found==generations.end()?(generations[m]=++nextGeneration):found->second;
        newer.actors.emplace(m,Actor{generation,m->x,m->y,m->z,m->health,m->angle});
    }
    for(int i=0;i<numsectors;++i) newer.planes[i]={sectors[i].floorheight,sectors[i].ceilingheight};
    for(int i=0;i<MAXPLAYERS;++i) {
        const player_t &p=players[i]; Camera &camera=newer.cameras[i];
        camera.actor=p.mo; camera.z=p.viewz; camera.weapon=p.readyweapon; camera.life=p.playerstate;
        std::copy_n(p.psprites,NUMPSPRITES,camera.sprites.begin());
    }
    if(!older.valid || older.time+1!=newer.time || older.clock+1!=newer.clock || older.episode!=newer.episode || older.map!=newer.map || older.planes.size()!=newer.planes.size()) older.valid=false;
}
void Native_RenderPrepare(void) {
    renderReady=enabled() && newer.valid && gamestate==GS_LEVEL;
    renderActors.clear();
    if(!renderReady) return;
    fraction=older.valid && !paused && !menuactive && Native_IsActive()?std::clamp(Native_PresentationTics()-newer.clock,0.0,1.0):1;
    renderSectors.resize(numsectors);
    for(int i=0;i<numsectors;++i) {
        sector_t &copy=renderSectors[i]; copy=sectors[i];
        if(older.valid) {
            const Plane &a=older.planes[i],&b=newer.planes[i];
            if(std::llabs(int64_t(b.floor)-a.floor)<=128LL*FRACUNIT) copy.floorheight=blend(a.floor,b.floor);
            if(std::llabs(int64_t(b.ceiling)-a.ceiling)<=128LL*FRACUNIT) copy.ceilingheight=blend(a.ceiling,b.ceiling);
        }
    }
    renderActors.reserve(newer.actors.size());
    for(const auto &[source,b]:newer.actors) {
        // A removed/recycled address cannot inherit the old actor's history.
        auto live=generations.find(source);
        if(live==generations.end() || live->second!=b.generation) continue;
        mobj_t copy=*source;
        auto prior=older.actors.find(source);
        if(older.valid && prior!=older.actors.end() && continuous(prior->second,b)) {
            const Actor &a=prior->second;
            copy.x=blend(a.x,b.x); copy.y=blend(a.y,b.y); copy.z=blend(a.z,b.z); copy.angle=blendAngle(a.angle,b.angle);
        }
        renderActors.emplace(source,copy);
    }
}
void *Native_RenderMobj(void *actor) {
    if(!renderReady) return actor;
    auto found=renderActors.find(static_cast<mobj_t *>(actor));
    return found==renderActors.end()?actor:&found->second;
}
void *Native_RenderSector(void *sector) {
    if(!renderReady || !sector) return sector;
    uintptr_t address=reinterpret_cast<uintptr_t>(sector),base=reinterpret_cast<uintptr_t>(sectors);
    if(address<base || address>=base+size_t(numsectors)*sizeof(sector_t)) return sector;
    return &renderSectors[(address-base)/sizeof(sector_t)];
}
void Native_RenderCamera(void *player,int32_t *x,int32_t *y,int32_t *z,uint32_t *angle) {
    if(!renderReady || !older.valid) return;
    auto *p=static_cast<player_t *>(player); int index=int(p-players);
    if(index<0 || index>=MAXPLAYERS) return;
    const Camera &a=older.cameras[index],&b=newer.cameras[index];
    auto pa=older.actors.find(a.actor),pb=newer.actors.find(b.actor);
    if(a.actor!=b.actor || a.life!=b.life || pa==older.actors.end() || pb==newer.actors.end() || !continuous(pa->second,pb->second)) return;
    *x=blend(pa->second.x,pb->second.x); *y=blend(pa->second.y,pb->second.y);
    if(std::llabs(int64_t(b.z)-a.z)<=64LL*FRACUNIT) *z=blend(a.z,b.z);
    *angle+=blendAngle(pa->second.angle,pb->second.angle)-pb->second.angle;
    // Local mouse and stick turning are shown at once: the last tic's turn
    // in full (not blended in over the next tic) plus turning not yet sent.
    if(index==consoleplayer && !demoplayback) {
        int64_t last=int64_t(native_mouseturn)+Native_ControllerLastTurn();
        *angle+=uint32_t(int64_t((1-fraction)*double(last*65536)))+uint32_t(G_NativePendingTurn())+uint32_t(Native_ControllerPendingTurn());
    }
}
void Native_RenderPSprite(void *player,int index,int32_t *x,int32_t *y) {
    if(!renderReady || !older.valid || index<0 || index>=NUMPSPRITES) return;
    int p=int(static_cast<player_t *>(player)-players); if(p<0 || p>=MAXPLAYERS) return;
    const Camera &a=older.cameras[p],&b=newer.cameras[p];
    auto pa=older.actors.find(a.actor),pb=newer.actors.find(b.actor);
    if(a.actor!=b.actor || a.life!=b.life || a.weapon!=b.weapon || pa==older.actors.end() || pb==newer.actors.end() || !continuous(pa->second,pb->second)) return;
    if(!a.sprites[index].state || !b.sprites[index].state) return;
    if(std::llabs(int64_t(b.sprites[index].sx)-a.sprites[index].sx)>64LL*FRACUNIT || std::llabs(int64_t(b.sprites[index].sy)-a.sprites[index].sy)>64LL*FRACUNIT) return;
    *x=blend(a.sprites[index].sx,b.sprites[index].sx); *y=blend(a.sprites[index].sy,b.sprites[index].sy);
}

double Native_RenderLevelTime(void) { return renderReady && older.valid?newer.time-1+fraction:leveltime; }
int Native_RenderSectorIndex(const void *sector) {
    auto address=reinterpret_cast<uintptr_t>(sector);
    auto base=reinterpret_cast<uintptr_t>(sectors);
    if(sectors && address>=base && address<base+size_t(numsectors)*sizeof(sector_t)) return int((address-base)/sizeof(sector_t));
    base=reinterpret_cast<uintptr_t>(renderSectors.data());
    if(!renderSectors.empty() && address>=base && address<base+renderSectors.size()*sizeof(sector_t)) return int((address-base)/sizeof(sector_t));
    return -1;
}
