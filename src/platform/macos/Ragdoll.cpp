// SPDX-License-Identifier: GPL-2.0-only
// Death physics (2026-10-09). The original game already pushes a corpse
// along the floor with the killing blow's momentum; this adds the part the
// simulation leaves out: the body is thrown up by the impulse, falls under
// DOOM's gravity, bounces with heavy damping and settles. Heavy monsters
// barely leave the ground; explosions throw bodies higher. The lift is a
// pure function of time since death evaluated at presentation time, so it is
// smooth at any frame rate, ends at the simulated position and never feeds
// back into game state, demos or saves.
#include "Native.h"
extern "C" {
#include "doomstat.h"
#include "p_local.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace {
struct Body { double start=0,speed=0; int deathTic=0; mobjtype_t type{}; };
std::unordered_map<const mobj_t *,Body> bodies;
// Damage taken this tic: a shotgun blast is one impulse of many pellets.
struct Recent { int tic=-1,damage=0; };
std::unordered_map<const mobj_t *,Recent> recent;
constexpr double Gravity=1.0;      // map units per tic², as P_ZMovement
constexpr double Restitution=.28;  // limp bodies keep little of their speed
constexpr double MaxLift=72;

// Hit reactions (2026-10-10). A body that survives a hit jolts away from
// it and recovers: the shove is the hit's impulse on the body's mass (the
// same damage*100/mass the game uses for knockback) plus the involuntary
// withdrawal flinch, which heavy bodies show less (~1/sqrt(mass)). Each hit
// is a critically damped response, peak ~57 ms after impact and settled in
// ~0.3 s; the reaction is scaled up (~2.2x physical) so every hit reads
// clearly at game distances. Overlapping hits add. Like the
// death lift this is evaluated at presentation time and never reaches game
// state.
struct Impulse { double start=0,dx=0,dy=0,amplitude=0; };
struct Flinch { std::array<Impulse,4> hits{}; unsigned next=0; int tic=0; mobjtype_t type{}; };
std::unordered_map<const mobj_t *,Flinch> flinches;
constexpr double FlinchPeak=2.0;   // tics to the farthest point (~57 ms)
constexpr double FlinchMax=9.0;    // map units (~27 cm): a hard stagger, not a step
double response(double t) { return t<=0?0:(t/FlinchPeak)*std::exp(1-t/FlinchPeak); }

// Height above the floor t tics after launch at speed v, with bounces.
double lift(double v,double t) {
    while(v>.6 && t>0) {
        double flight=2*v/Gravity;
        if(t<flight) return v*t-.5*Gravity*t*t;
        t-=flight; v*=Restitution;
    }
    return 0;
}
}

void Native_BodyHit(void *targetPointer,void *inflictorPointer,void *sourcePointer,int damage) {
    auto *t=static_cast<mobj_t *>(targetPointer);
    auto *inflictor=static_cast<mobj_t *>(inflictorPointer);
    auto *source=static_cast<mobj_t *>(sourcePointer);
    if(!t || damage<=0 || t->health<=0 || t->player || gamestate!=GS_LEVEL) return;
    if(recent.size()>256) recent.clear();
    Recent &r=recent[t];
    if(r.tic!=leveltime) r={leveltime,0};
    r.damage+=damage;
    if(t->health-damage>0) {
        const mobj_t *from=inflictor?inflictor:source;
        if(!from || !t->info || t->info->mass<=0 || (t->flags&MF_NOGRAVITY)) return;
        double dx=(t->x-from->x)*(1.0/FRACUNIT),dy=(t->y-from->y)*(1.0/FRACUNIT);
        if(inflictor && (inflictor->flags&MF_MISSILE) && (inflictor->momx || inflictor->momy)) { dx=inflictor->momx; dy=inflictor->momy; }
        double length=std::hypot(dx,dy); if(length<1e-6) return;
        double mass=t->info->mass,amplitude=std::min(FlinchMax,2.0*std::sqrt(100/mass)+.1*std::min(damage,200)*100/mass);
        if(flinches.size()>256) flinches.clear();
        Flinch &f=flinches[t];
        if(f.type!=t->type) f=Flinch{};
        f.type=t->type; f.tic=leveltime;
        f.hits[f.next++%f.hits.size()]={Native_PresentationTics(),dx/length,dy/length,amplitude};
        return;
    }
    damage=r.damage; recent.erase(t);
    if(!(t->flags&MF_SHOOTABLE) || (t->flags&MF_NOGRAVITY) || !t->info || t->info->mass<=0) return;
    // Melee and hitscan push mostly sideways; splash damage lifts from below.
    bool blast=inflictor && inflictor!=source && !(inflictor->flags&MF_MISSILE);
    bool missile=inflictor && (inflictor->flags&MF_MISSILE);
    double impulse=std::min(damage,400)*100.0/t->info->mass;
    double height=std::min(MaxLift,impulse*(blast?.6:missile?.4:.28));
    if(height<3) return;
    Body b; b.start=Native_PresentationTics(); b.speed=std::sqrt(2*Gravity*height); b.deathTic=leveltime; b.type=t->type;
    bodies[t]=b;
}
int32_t Native_BodyLift(const void *actor) {
    if(bodies.empty()) return 0;
    auto found=bodies.find(static_cast<const mobj_t *>(actor));
    if(found==bodies.end()) return 0;
    const auto *t=found->first; const Body &b=found->second;
    // Resurrected, reused after a load, or long settled: forget it.
    if(gamestate!=GS_LEVEL || t->type!=b.type || t->health>0 || leveltime<b.deathTic || leveltime-b.deathTic>4*TICRATE) {
        bodies.erase(found); return 0;
    }
    double h=lift(b.speed,Native_PresentationTics()-b.start);
    return h>0?int32_t(h*FRACUNIT):0;
}
void Native_BodyFlinch(const void *actor,int32_t *x,int32_t *y) {
    *x=*y=0;
    if(flinches.empty()) return;
    auto found=flinches.find(static_cast<const mobj_t *>(actor));
    if(found==flinches.end()) return;
    const auto *t=found->first; const Flinch &f=found->second;
    if(gamestate!=GS_LEVEL || t->type!=f.type || t->health<=0 || leveltime<f.tic || leveltime-f.tic>TICRATE) { flinches.erase(found); return; }
    double now=Native_PresentationTics(),ox=0,oy=0;
    for(const Impulse &i:f.hits) if(i.amplitude>0) { double k=i.amplitude*response(now-i.start); ox+=i.dx*k; oy+=i.dy*k; }
    double length=std::hypot(ox,oy);
    if(length>FlinchMax) { ox*=FlinchMax/length; oy*=FlinchMax/length; }
    *x=int32_t(ox*FRACUNIT); *y=int32_t(oy*FRACUNIT);
}
void Native_BodyForget(const void *actor) {
    bodies.erase(static_cast<const mobj_t *>(actor)); recent.erase(static_cast<const mobj_t *>(actor)); flinches.erase(static_cast<const mobj_t *>(actor));
}
