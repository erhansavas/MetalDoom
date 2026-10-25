// SPDX-License-Identifier: GPL-2.0-only
// Cosmetic gore simulation: directional impact sprays, ballistic droplets and
// flesh chunks that collide with the map, wall/floor/ceiling stains, growing
// corpse pools and crush/gib bursts. It reads game state at 35 Hz and never
// writes it, creates thinkers or calls the gameplay RNG.
#include "Native.h"
#include "Gore.hpp"
#include "GoreSound.hpp"
extern "C" {
#include "doomstat.h"
#include "p_local.h"
#include "r_local.h"
#include "w_wad.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <mutex>
#include <unordered_map>

namespace gore {
namespace {
State shared;
uint32_t seed=0x6d2b79f5u;
float random01() { seed=seed*1664525u+1013904223u; return float(seed>>8)/16777216.0f; }
float range(float low,float high) { return low+(high-low)*random01(); }
constexpr double Unit=1.0/FRACUNIT;

// Line queries walk the blockmap with a private stamp; validcount is untouched.
std::vector<uint32_t> lineStamp; uint32_t stamp=0;
template<class Visit> void linesNear(double x0,double y0,double x1,double y1,Visit visit) {
    if(!blockmap || bmapwidth<=0 || bmapheight<=0) return;
    if(lineStamp.size()!=size_t(numlines)) lineStamp.assign(numlines,0);
    if(++stamp==0) { std::fill(lineStamp.begin(),lineStamp.end(),0); stamp=1; }
    double ox=bmaporgx*Unit,oy=bmaporgy*Unit;
    int bx0=std::max(0,int(std::floor((std::min(x0,x1)-ox)/128))),bx1=std::min(bmapwidth-1,int(std::floor((std::max(x0,x1)-ox)/128)));
    int by0=std::max(0,int(std::floor((std::min(y0,y1)-oy)/128))),by1=std::min(bmapheight-1,int(std::floor((std::max(y0,y1)-oy)/128)));
    for(int by=by0;by<=by1;++by) for(int bx=bx0;bx<=bx1;++bx) {
        for(const short *list=blockmaplump+blockmap[by*bmapwidth+bx];*list!=-1;++list) {
            int index=*list; if(index<0 || index>=numlines || lineStamp[index]==stamp) continue;
            lineStamp[index]=stamp; visit(&lines[index],index);
        }
    }
}
int sectorAt(double x,double y) {
    if(!numnodes && !numsubsectors) return -1;
    subsector_t *sub=R_PointInSubsector(fixed_t(std::lround(x*FRACUNIT)),fixed_t(std::lround(y*FRACUNIT)));
    return sub && sub->sector?int(sub->sector-sectors):-1;
}
// Water, nukage, slime, lava and blood-pool floors wash blood away: nothing
// stains them and a sole that steps in comes out clean.
bool liquidFloor(int s) {
    if(s<0) return false;
    const char *name=lumpinfo[firstflat+sectors[s].floorpic].name;
    for(const char *prefix:{"FWATER","NUKAGE","SLIME","LAVA","BLOOD"})
        if(!strncasecmp(name,prefix,strlen(prefix))) return true;
    return false;
}
double floorOf(int s) { return sectors[s].floorheight*Unit; }
double ceilingOf(int s) { return sectors[s].ceilingheight*Unit; }

struct Hit { double t=2; int line=-1,side=0; double s=0,z=0; bool sky=false; };
// First line that stops a path at the height it crosses.
Hit trace(double x0,double y0,double z0,double x1,double y1,double z1) {
    Hit best;
    linesNear(x0-2,y0-2,x1+2,y1+2,[&](line_t *ld,int index) {
        double ax=ld->v1->x*Unit,ay=ld->v1->y*Unit,lx=ld->dx*Unit,ly=ld->dy*Unit;
        double px=x1-x0,py=y1-y0,den=px*ly-py*lx;
        if(std::fabs(den)<1e-9) return;
        double t=((ax-x0)*ly-(ay-y0)*lx)/den,s=((ax-x0)*py-(ay-y0)*px)/den;
        if(t<0 || t>1 || s<0 || s>1 || t>=best.t) return;
        int side=((x0-ax)*ly-(y0-ay)*lx)>0?0:1;
        double z=z0+(z1-z0)*t;
        if(ld->sidenum[side]<0) return;
        bool blocked=!(ld->flags&ML_TWOSIDED) || ld->sidenum[side^1]<0,sky=false;
        if(!blocked) {
            const sector_t *other=sides[ld->sidenum[side^1]].sector;
            if(z<other->floorheight*Unit) blocked=true;
            else if(z>other->ceilingheight*Unit) { blocked=true; sky=other->ceilingpic==skyflatnum; }
        }
        if(blocked) best={t,index,side,s,z,sky};
    });
    return best;
}
double lineLength(const line_t *ld) { return std::hypot(ld->dx*Unit,ld->dy*Unit); }

// Clearance to the nearest wall or step so a stain never bleeds through walls.
double clearance(double x,double y,double radius,bool ceiling) {
    linesNear(x-radius,y-radius,x+radius,y+radius,[&](line_t *ld,int) {
        bool open=(ld->flags&ML_TWOSIDED) && ld->frontsector && ld->backsector &&
            (ceiling?ld->frontsector->ceilingheight==ld->backsector->ceilingheight:ld->frontsector->floorheight==ld->backsector->floorheight);
        if(open) return;
        double ax=ld->v1->x*Unit,ay=ld->v1->y*Unit,lx=ld->dx*Unit,ly=ld->dy*Unit,len=lx*lx+ly*ly;
        double t=len>0?std::clamp(((x-ax)*lx+(y-ay)*ly)/len,0.0,1.0):0;
        radius=std::min(radius,std::hypot(x-ax-t*lx,y-ay-t*ly));
    });
    return radius;
}

void unlinkWall(int index) {
    WallDecal &d=shared.walls[index];
    int slot=d.line*2+d.side;
    if(slot<0 || size_t(slot)>=shared.sideHead.size()) return;
    for(int32_t *link=&shared.sideHead[slot];*link>=0;link=&shared.walls[*link].next)
        if(*link==index) { *link=d.next; break; }
}
void ensureLevel() {
    if(shared.sideHead.size()!=size_t(numlines)*2) {
        shared.sideHead.assign(size_t(numlines)*2,-1);
        for(auto &d:shared.walls) d.active=false;
    }
}

// Stain layers per 16-unit cell; small splats stop stacking where the floor
// is already soaked, which bounds overdraw in heavy fights.
std::unordered_map<uint64_t,uint16_t> layers;
uint64_t layerKey(double x,double y,bool ceiling) {
    return (uint64_t(uint32_t(int32_t(std::floor(x/16))))<<33)^(uint64_t(uint32_t(int32_t(std::floor(y/16))))<<1)^uint64_t(ceiling);
}
void addWall(const Hit &hit,double radius,double stretch,uint8_t fluid,double drip);
struct CeilingDrip { int32_t index=-1,born=0,next=0,left=0; };
std::vector<CeilingDrip> ceilingDrips;
// dx,dy: travel of the drop that made the stain (zero: no preferred axis);
// stretch elongates it along that travel.
void addPlane(double x,double y,int sector,double radius,bool pool,uint8_t fluid,bool ceiling,int grow,
              double dx=0,double dy=0,double stretch=1) {
    if(sector<0 || radius<.8 || (!ceiling && liquidFloor(sector))) return;
    if(!pool && radius<6) { auto found=layers.find(layerKey(x,y,ceiling)); if(found!=layers.end() && found->second>=10) return; }
    double room=clearance(x,y,radius*stretch,ceiling);
    if(room<radius*stretch) { stretch=std::max(1.0,room/radius); radius=std::min(radius,room); }
    if(radius<.8) return;
    // Repeated drops on one spot thicken an existing stain rather than stack.
    for(unsigned back=1;back<=48;++back) {
        PlaneDecal &e=shared.planes[(shared.planeCursor-back+PlaneCount)%PlaneCount];
        if(!e.active || e.sector!=sector || e.ceiling!=ceiling) continue;
        double distance=std::hypot(e.x-x,e.y-y);
        if(distance+radius*.6<e.radius) { if(!e.grow) e.radius=float(std::min<double>(e.radius+radius*.08,clearance(e.x,e.y,e.radius*1.04,ceiling))); return; }
    }
    PlaneDecal &d=shared.planes[shared.planeCursor++%PlaneCount];
    if(d.active) { auto found=layers.find(layerKey(d.x,d.y,d.ceiling)); if(found!=layers.end() && !--found->second) layers.erase(found); }
    ++layers[layerKey(x,y,ceiling)];
    double length=std::hypot(dx,dy);
    float angle=length>1e-6?float(std::atan2(dy,dx)):range(0,6.2832f);
    d={float(x),float(y),float(radius),std::cos(angle),std::sin(angle),float(stretch),sector,leveltime,uint16_t(grow),
       uint8_t(pool?random01()*PoolShapes:random01()*SplatShapes),fluid,uint8_t(ceiling),uint8_t(random01()*3),true};
    if(pool) d.shape|=PoolBit;
    // A ceiling stain that reaches a wall runs down it: one-sided walls and
    // the face down to a lower neighbouring ceiling. The run reaches the
    // floor or drips off that face's lower edge like any wall drip.
    if(ceiling && radius>=2.5) {
        double best=radius*stretch+1; int line=-1,side=0; double along=0;
        linesNear(x-best,y-best,x+best,y+best,[&](line_t *ld,int) {
            int own=ld->frontsector==&sectors[sector]?0:ld->backsector==&sectors[sector]?1:-1;
            if(own<0) return;
            if((ld->flags&ML_TWOSIDED) && ld->frontsector && ld->backsector) {
                const sector_t *other=own?ld->frontsector:ld->backsector;
                if(other->ceilingheight>=sectors[sector].ceilingheight || other->ceilingpic==skyflatnum) return;
            }
            double ax=ld->v1->x*Unit,ay=ld->v1->y*Unit,lx=ld->dx*Unit,ly=ld->dy*Unit,len=lx*lx+ly*ly;
            double t=len>0?std::clamp(((x-ax)*lx+(y-ay)*ly)/len,0.0,1.0):0,dist=std::hypot(x-ax-t*lx,y-ay-t*ly);
            if(dist<best) { best=dist; line=int(ld-lines); side=own; along=t; }
        });
        if(line>=0) {
            Hit edge; edge.t=0; edge.line=line; edge.side=side; edge.s=along; edge.z=ceilingOf(sector)-1;
            addWall(edge,std::min(radius*.45,8.0),1,fluid,std::min(5+radius*.8,18.0));
        }
    }
    // A wet ceiling stain sheds a few drops while it is fresh.
    if(ceiling && radius>=2 && ceilingDrips.size()<256)
        ceilingDrips.push_back({int32_t((shared.planeCursor-1)%PlaneCount),leveltime,leveltime+int(range(15,40)),6+int(std::min(radius,15.0)*range(.6f,1.2f))});
}
void addWall(const Hit &hit,double radius,double stretch,uint8_t fluid,double drip) {
    if(hit.line<0 || hit.sky || radius<.6) return;
    // A wall stain stays a splash, never a sheet: its size is capped and blood
    // beyond it lands as more stains (and runs) elsewhere.
    radius=std::min(radius,12.0); stretch=std::min(stretch,1.7);
    ensureLevel();
    const line_t *ld=&lines[hit.line];
    double length=lineLength(ld),u=(hit.side?1-hit.s:hit.s)*length;
    int anchor=-1; bool ceiling=false; double base=0;
    if((ld->flags&ML_TWOSIDED) && ld->sidenum[hit.side^1]>=0) {
        const sector_t *other=sides[ld->sidenum[hit.side^1]].sector;
        anchor=int(other-sectors);
        ceiling=hit.z>other->ceilingheight*Unit;
        base=ceiling?other->ceilingheight*Unit:other->floorheight*Unit;
    }
    int slot=hit.line*2+hit.side;
    for(int32_t i=shared.sideHead[slot];i>=0;i=shared.walls[i].next) {
        WallDecal &e=shared.walls[i];
        if(std::fabs(e.u-u)+radius*.5<e.radius && std::fabs(e.z+(e.anchor>=0?(e.anchorCeiling?ceilingOf(e.anchor):floorOf(e.anchor)):0)-hit.z)+radius*.5<e.radius) {
            e.radius=float(std::min(e.radius+radius*.1,14.0));
            if(drip>e.drip) { e.drip=float(drip); e.landed=0; }
            return;
        }
    }
    int index=int(shared.wallCursor++%WallCount);
    if(shared.walls[index].active) unlinkWall(index);
    WallDecal &d=shared.walls[index];
    d={float(u),float(hit.z-base),float(radius),float(stretch),float(drip),hit.line,anchor,leveltime,shared.sideHead[slot],
       uint8_t(hit.side),uint8_t(random01()*SplatShapes),fluid,uint8_t(random01()*3),uint8_t(ceiling),0,true};
    shared.sideHead[slot]=index;
}

Particle *spawn(double x,double y,double z,double vx,double vy,double vz,float size,int life,uint8_t chunk,uint8_t fluid) {
    int sector=sectorAt(x,y);
    if(sector<0) return nullptr;
    z=std::clamp(z,floorOf(sector)+.5,ceilingOf(sector)-.5);
    if(z<floorOf(sector)) return nullptr;
    Particle &p=shared.particles[shared.particleCursor++%ParticleCount];
    p={float(x),float(y),float(z),float(x),float(y),float(z),float(vx),float(vy),float(vz),size,sector,0,uint16_t(life),chunk,fluid,uint8_t(random01()*3),0,0,0,true};
    return &p;
}
// A steady stream, like water from a tap: drops leave the wound every tic
// at nearly the same point and speed, and the renderer draws each as a
// streak reaching back to the previous one (Particle.spread=1), so the
// column reads unbroken. flow 0..1 sets the outflow speed and thickness.
void pour(double x,double y,double z,double ux,double uy,double flow,uint8_t fluid) {
    for(int k=0;k<1+(flow>.55);++k) {
        double out=.2+.55*flow;
        if(Particle *p=spawn(x+range(-.25f,.25f),y+range(-.25f,.25f),z-k*.4,ux*out,uy*out,-.05-.1*k,float(.26+.22*flow),150,0,fluid)) { p->spread=1; p->tone=0; }
    }
}
// A cone of droplets around a unit direction. Speeds skew low, so most blood
// lands close to the wound and only a few drops carry far; the cone is much
// flatter vertically than horizontally, as spatter leaves a body.
void spray(double x,double y,double z,double dx,double dy,double dz,int count,float spread,float slow,float fast,
           float small,float large,int life,uint8_t fluid,float lift=0,uint8_t chunk=0) {
    for(int i=0;i<count;++i) {
        float yaw=range(-spread,spread),pitch=range(-spread,spread)*.35f,r=random01(),speed=slow+(fast-slow)*r*r;
        double cx=dx*std::cos(yaw)-dy*std::sin(yaw),cy=dx*std::sin(yaw)+dy*std::cos(yaw);
        spawn(x,y,z,cx*speed,cy*speed,(dz+pitch)*speed+lift*random01(),range(small,large),life,chunk,fluid);
    }
}

uint8_t fluidOf(const mobj_t *m) {
    if(m->type==MT_HEAD) return Blue;          // the cacodemon's death art bleeds blue
    if(m->type==MT_BRUISER || m->type==MT_KNIGHT) return Green;
    return Red;
}

struct Pending { const mobj_t *target=nullptr; int tic=-1; double x=0,y=0,z=0,dx=1,dy=0,dz=0; } pending;
struct Budget { const mobj_t *target=nullptr; int tic=-1,used=0; } budget;
int allow(const mobj_t *target,int wanted) {
    if(budget.target!=target || budget.tic!=leveltime) budget={target,leveltime,0};
    int granted=std::clamp(480-budget.used,0,wanted); budget.used+=granted; return granted;
}

int spurtSound=-1,slapSound=-1,dripSound=-1;
struct Bleeder { const mobj_t *mo=nullptr; int start=0,pool=-1,beat=0; bool gib=false; float lastx=0,lasty=0,dx=1,dy=0; };
std::vector<Bleeder> bleeders;

// Dismemberment follows the body plan. Each piece leaves from where it sits
// on the body (fractions of actor height), with a speed that falls with its
// mass (equal impulse share, v ~ 1/sqrt(m)): the head and limbs fly short and
// heavy, viscera spill low and close, bone splinters scatter fast and far.
// Bipeds (troopers, imps, demons, barons, the player) shed a head, two arms
// and two legs; other bodies only break into tissue, organs and bone.
bool biped(const mobj_t *t) {
    switch(t->type) {
    case MT_PLAYER: case MT_POSSESSED: case MT_SHOTGUY: case MT_CHAINGUY: case MT_WOLFSS: case MT_TROOP:
    case MT_SERGEANT: case MT_SHADOWS: case MT_KNIGHT: case MT_BRUISER: case MT_UNDEAD: case MT_FATSO: case MT_BABY: return true;
    default: return false;
    }
}
void piece(const mobj_t *t,double heightFraction,double size,double massKg,uint8_t kind,uint8_t fluid,double strength,int life) {
    double h=t->height*Unit,r=t->radius*Unit,x=t->x*Unit,y=t->y*Unit,z=t->z*Unit+h*heightFraction;
    float a=range(0,6.2832f);
    double speed=std::clamp(9.0*strength/std::sqrt(std::max(massKg,.05)),1.0,12.0)*range(.75f,1.15f);
    double up=std::clamp(speed*range(.45f,.9f)+(heightFraction>.8?1.5:0),1.0,10.0);
    spawn(x+std::cos(a)*r*.4,y+std::sin(a)*r*.4,z,std::cos(a)*speed,std::sin(a)*speed,up,float(size),life,kind,fluid);
}
void gibs(const mobj_t *t,uint8_t fluid,double strength) {
    double x=t->x*Unit,y=t->y*Unit,z=t->z*Unit+std::max(16.0,t->height*Unit*.5);
    double scale=std::clamp(t->radius*Unit/20.0,.8,2.0); // 20 units: a human-sized body
    int drops=allow(t,int(110*strength)),mist=allow(t,40);
    if(biped(t) && allow(t,5)==5) {
        piece(t,.92,1.5*scale,5,Head,fluid,strength,500);
        for(int i=0;i<2;++i) piece(t,.68,1.25*scale,4,Limb,fluid,strength,500);
        for(int i=0;i<2;++i) piece(t,.28,1.6*scale,11,Limb,fluid,strength,500);
    }
    int organs=allow(t,int(6+4*strength)),bones=allow(t,int(8+6*strength)),tissue=allow(t,int(10+8*strength));
    for(int i=0;i<organs;++i) piece(t,range(.35f,.6f),range(.8f,1.3f)*scale,range(.8f,2.5f),Viscera,fluid,strength*.6,450);
    for(int i=0;i<bones;++i) piece(t,range(.15f,.9f),range(.3f,.6f)*scale,range(.05f,.25f),Bone,fluid,strength*.8,400);
    for(int i=0;i<tissue;++i) piece(t,range(.2f,.85f),range(.6f,1.4f)*scale,range(.3f,1.5f),Flesh,fluid,strength,400);
    for(int i=0;i<drops;++i) {
        float a=range(0,6.2832f),r=random01(),speed=1+6*r*r;
        spawn(x,y,z+range(-10,10),std::cos(a)*speed,std::sin(a)*speed,range(.5f,4.5f),range(.15f,.5f),160,0,fluid);
    }
    for(int i=0;i<mist;++i) {
        float a=range(0,6.2832f),speed=range(.5f,3);
        spawn(x,y,z+range(-6,6),std::cos(a)*speed,std::sin(a)*speed,range(-.5f,2),range(.08f,.2f),int(range(10,24)),0,fluid);
    }
}
// dx,dy: unit travel of the killing hit; the opened vessel faces its exit.
void addBleeder(const mobj_t *mo,bool gib,double dx,double dy) {
    for(auto &b:bleeders) if(b.mo==mo) { b.gib|=gib; return; }
    if(bleeders.size()>=160) bleeders.erase(bleeders.begin());
    bleeders.push_back({mo,leveltime,-1,leveltime+2,gib,float(mo->x*Unit),float(mo->y*Unit),float(dx),float(dy)});
}

// Open wounds on a body that is still alive (2026-10-10): it keeps
// bleeding from where it was hit, a few drops every few tics and short
// squirts while the wound is fresh, so a hurt monster leaves a trail. The
// wound turns with the body; it closes over 2-6 s by damage.
struct Wound { const mobj_t *mo=nullptr; int start=0,until=0; float height=.6f,angle=0; bool throat=false; };
std::vector<Wound> wounds;
void addWound(const mobj_t *t,double z,double dx,double dy,int damage) {
    if(t->player || damage<6) return;
    double h=t->height*Unit;
    float height=float(std::clamp((z-t->z*Unit)/std::max(h,1.0),.2,.9));
    float angle=float(std::atan2(dy,dx)-t->angle*(M_PI/2147483648.0));
    // Location matters (as in RDR2): a neck wound (upper fifth) pours far
    // longer and harder than one in a limb or the torso.
    bool throat=height>.74f && height<.9f;
    int until=leveltime+std::min(throat?420:240,(throat?140:80)+damage*5);
    for(auto &w:wounds) if(w.mo==t) { w={t,leveltime,std::max(until,w.until),height,angle,throat||w.throat}; return; }
    if(wounds.size()>=96) wounds.erase(wounds.begin());
    wounds.push_back({t,leveltime,until,height,angle,throat});
}
void stepWounds() {
    for(size_t i=0;i<wounds.size();) {
        Wound &w=wounds[i]; const mobj_t *mo=w.mo;
        if(mo->health<=0 || leveltime>=w.until) { w=wounds.back(); wounds.pop_back(); continue; }
        double left=double(w.until-leveltime)/std::max(1,w.until-w.start),a=w.angle+mo->angle*(M_PI/2147483648.0);
        double ux=std::cos(a),uy=std::sin(a),r=mo->radius*Unit*.6,x=mo->x*Unit+ux*r,y=mo->y*Unit+uy*r,z=mo->z*Unit+mo->height*Unit*w.height;
        uint8_t fluid=fluidOf(mo);
        double flow=left*(w.throat?1:.6);
        if(flow>.15) pour(x,y,z,ux,uy,flow,fluid);
        else if((leveltime+int(hash(uintptr_t(mo))&7))%std::max(2,int(7-5*left))==0)
            spawn(x,y,z,ux*range(.1f,.5f)+mo->momx*Unit*.5,uy*range(.1f,.5f)+mo->momy*Unit*.5,range(-.3f,.2f),range(.2f,.38f),150,0,fluid);
        if(leveltime-w.start<35 && (leveltime-w.start)%12==3)
            for(int k=0;k<int(3+5*left);++k) { float s=range(1,2.6f)*float(left);
                spawn(x,y,z,ux*s+range(-.2f,.2f),uy*s+range(-.2f,.2f),s*range(.2f,.45f),range(.18f,.34f),150,0,fluid); }
        ++i;
    }
}

// One impact: exit spray through the body, back spatter and mist toward the
// source, wound drips and stains on whatever wall lies behind the target.
void impact(const mobj_t *t,double x,double y,double z,double dx,double dy,double dz,int damage,bool corpse,bool melee) {
    uint8_t fluid=fluidOf(t);
    double k=std::clamp(damage/10.0,.4,8.0);
    if(t->player) {
        // Never in front of a camera: drips from the wound to the floor.
        int count=allow(t,int(3+2*k));
        for(int i=0;i<count;++i) {
            float a=range(0,6.2832f),r=t->radius*Unit*range(.9f,1.2f);
            spawn(t->x*Unit+std::cos(a)*r,t->y*Unit+std::sin(a)*r,t->z*Unit+range(18,40),std::cos(a)*range(.2f,1),std::sin(a)*range(.2f,1),range(-.5f,.5f),range(.2f,.4f),120,0,fluid);
        }
        return;
    }
    double length=std::sqrt(dx*dx+dy*dy+dz*dz); if(length<1e-6) { dx=1; dy=dz=0; length=1; }
    dx/=length; dy/=length; dz/=length;
    double through=t->radius*Unit*1.4;
    if(corpse) k=std::min(k,1.2);
    double speed=melee?.6:1.0;
    double ex=x+dx*through,ey=y+dy*through,ez=z+dz*through;
    // Exit wound: a dense cone of drops, then a fine high-velocity aerosol
    // that hangs a moment; back spatter and mist toward the shooter.
    spray(ex,ey,ez,dx,dy,dz-.05,allow(t,int(std::min(120.0,12+12*k))),.42f,
          float(1.8*speed),float(std::min(6.0+k*.6,10.0)*speed),.15f,float(std::min(.5+k*.03,.75)),150,fluid,.4f);
    spray(ex,ey,ez,dx,dy,dz,allow(t,int(std::min(60.0,6+5*k))),.5f,2,5,.06f,.14f,int(range(14,26)),fluid,.2f);
    spray(x,y,z,-dx,-dy,-dz+.05,allow(t,int(4+3*k)),.6f,.8f,2.8f,.15f,.38f,150,fluid);
    spray(x,y,z,-dx,-dy,0,allow(t,int(4+4*k)),.9f,.6f,2.4f,.08f,.18f,int(range(10,22)),fluid);
    spray(ex,ey,z,dx,dy,0,allow(t,int(3+3*k)),1.1f,.6f,2.4f,.08f,.18f,int(range(10,22)),fluid);
    int drips=allow(t,int(3+1.5*k));
    for(int i=0;i<drips;++i) spawn(x+range(-3,3),y+range(-3,3),z,range(-.3f,.3f),range(-.3f,.3f),range(-.5f,.3f),range(.2f,.4f),150,0,fluid);
    if(corpse) {
        for(auto &b:bleeders) if(b.mo==t && b.pool>=0) {
            PlaneDecal &pool=shared.planes[b.pool];
            if(pool.active && pool.sector>=0) pool.radius=float(std::min<double>(pool.radius*1.06,clearance(pool.x,pool.y,pool.radius*1.06,false)));
        }
        return;
    }
    // Heavy hits tear tissue out of the exit wound and splinter bone.
    if(!melee && k>=3) {
        int tissue=allow(t,int(1+k*.4)),bone=k>=5?allow(t,int(1+k*.25)):0;
        for(int i=0;i<tissue+bone;++i) {
            float yaw=range(-.4f,.4f),v=range(2.5f,6)*(i>=tissue?1.2f:1);
            double cx=dx*std::cos(yaw)-dy*std::sin(yaw),cy=dx*std::sin(yaw)+dy*std::cos(yaw);
            spawn(ex,ey,ez,cx*v,cy*v,range(.5f,2.5f),i>=tissue?range(.3f,.55f):range(.5f,1.1f),400,i>=tissue?Bone:Flesh,fluid);
        }
    }
    // A hard hit throws part of the spray upward: stains on the ceiling over
    // the exit wound, nearer and larger the lower the ceiling.
    if(!melee && k>=1.5) {
        int sector=sectorAt(ex,ey);
        if(sector>=0 && sectors[sector].ceilingpic!=skyflatnum) {
            double up=ceilingOf(sector)-ez,reachUp=40+22*k;
            for(int n=up<reachUp?1+int(std::min(k,6.0)*.5*random01()):0;n>0;--n) {
                double go=up*range(.2f,.9f),cx=ex+dx*go+range(-6,6),cy=ey+dy*go+range(-6,6);
                int s=sectorAt(cx,cy);
                if(s<0 || sectors[s].ceilingpic==skyflatnum || trace(ex,ey,ez,cx,cy,ez).line>=0) continue;
                addPlane(cx,cy,s,(2+1.2*k)*(1-up/reachUp*.6)*range(.5f,1),false,fluid,true,int(range(50,110)),dx,dy,range(1,1.8f));
            }
        }
    }
    // Back spatter: a fan of spray onto whatever lies behind the target.
    // The core lands as one large stain with runs; the edges of the cone
    // throw smaller, more stretched stains, closer ones larger.
    double reach=std::min(56+26*k,260.0);
    int rays=std::min(10,3+int(k));
    for(int r=0;r<rays;++r) {
        double yaw=r?range(-.45f,.45f):0,pitch=r?range(-.25f,.25f):0;
        double cx=dx*std::cos(yaw)-dy*std::sin(yaw),cy=dx*std::sin(yaw)+dy*std::cos(yaw),cz=dz+pitch,go=reach*(r?range(.6f,1):1);
        Hit hit=trace(ex,ey,ez,ex+cx*go,ey+cy*go,ez+cz*go);
        if(hit.line<0) continue;
        const line_t *ld=&lines[hit.line];
        double nx=ld->dy*Unit,ny=-ld->dx*Unit,nl=std::hypot(nx,ny),facing=nl>0?std::fabs(cx*nx+cy*ny)/nl:1;
        double radius=(5+2.6*k)*(1-hit.t*.55)*(r?range(.25f,.6f):1);
        addWall(hit,radius,std::clamp(1/std::max(facing,.35),1.0,2.6),fluid,r==0?(k>1.2?6+3.5*k:0):radius>4?4+radius:0);
    }
}


void stepParticle(Particle &p) {
    p.px=p.x; p.py=p.y; p.pz=p.z;
    if(p.resting==2) {
        // A bead gathering under a ceiling stain: it swells until its
        // weight tears it free, then falls as a ceiling drip.
        if(p.sector<0 || ++p.age>=p.life) { p.active=false; return; }
        p.size=std::min(p.size+.012f,.42f);
        p.z=p.pz=float(ceilingOf(p.sector)-p.size*.6);
        if(p.age>=p.opacity) { p.resting=0; p.age=0; p.life=400; p.vz=-.05f; }
        return;
    }
    if(p.resting) {
        if(p.sector>=0) p.z=p.pz=float(floorOf(p.sector)+p.size*.5);
        if(++p.age>=p.life) { p.active=false; addPlane(p.x,p.y,p.sector,p.size*2.2,false,p.fluid,false,0); }
        return;
    }
    bool mist=!p.chunk && p.life<40;
    // Severed parts bleed as they fly: a thin trail from the open wound.
    if((p.chunk==Head || p.chunk==Limb) && (p.age&1) && shared.activeParticles<ParticleCount*2/3)
        spawn(p.x,p.y,p.z,p.vx*.2,p.vy*.2,p.vz*.2,range(.12f,.22f),120,0,p.fluid);
    // One map unit is about 3 cm: real gravity is ~0.26 units/tic^2. Air
    // drag grows as a drop shrinks, so fine spray stops short.
    p.vz-=p.chunk?.42f:mist?.05f:.28f;
    float drag=mist?.86f:p.chunk?.99f:1-.0035f/std::max(p.size,.1f);
    p.vx*=drag; p.vy*=drag; if(mist) p.vz*=.9f; else p.vz*=drag;
    double nx=p.x+p.vx,ny=p.y+p.vy,nz=p.z+p.vz;
    if(std::fabs(p.vx)+std::fabs(p.vy)>1e-4) {
        Hit hit=trace(p.x,p.y,p.z,nx,ny,nz);
        if(hit.line>=0) {
            if(hit.sky) { p.active=false; return; }
            const line_t *ld=&lines[hit.line];
            double speed=std::hypot(p.vx,p.vy);
            if(!mist || random01()<.25f) {
                double lx=ld->dx*Unit,ly=ld->dy*Unit,ll=std::hypot(lx,ly),along=ll>0?std::fabs(p.vx*lx+p.vy*ly)/ll/std::max(speed,.01):0;
                // Only drops with enough volume run; fine spray just stains.
                addWall(hit,(mist?.6:1.0)+p.size*(mist?2:3.5+std::min(speed,8.0)*.6)*(p.chunk?1.4:1),1+along*1.5,p.fluid,p.size>.34 && !mist?4+speed*1.5:0);
            }
            if(!p.chunk) { p.active=false; return; }
            double lx=ld->dx*Unit,ly=ld->dy*Unit,ll=std::hypot(lx,ly);
            double nxn=ly/ll,nyn=-lx/ll,dot=p.vx*nxn+p.vy*nyn;
            p.vx=float((p.vx-2*dot*nxn)*.35); p.vy=float((p.vy-2*dot*nyn)*.35);
            nx=p.x+(nx-p.x)*hit.t*.9; ny=p.y+(ny-p.y)*hit.t*.9;
        }
    }
    int sector=sectorAt(nx,ny);
    if(sector<0) { p.active=false; return; }
    p.sector=sector; p.x=float(nx); p.y=float(ny);
    double floor=floorOf(sector),ceiling=ceilingOf(sector);
    if(nz<=floor) {
        double across=std::hypot(p.vx,p.vy),down=std::fabs(p.vz),impact=down+across*.5;
        if(!p.chunk && p.spread==2) {
            // Ceiling drip: each drop widens the puddle it falls into.
            bool merged=false;
            for(auto &e:shared.planes) if(e.active && !e.ceiling && e.sector==sector && !(e.shape&PrintBit) && std::hypot(e.x-nx,e.y-ny)<e.radius) {
                e.radius=float(std::min<double>({e.radius+.7,24.0,clearance(e.x,e.y,e.radius+.7,false)})); merged=true; break;
            }
            if(!merged) addPlane(nx,ny,sector,2.6,true,p.fluid,false,20);
            // The impact throws a crown of droplets that land as specks.
            int crown=3+int(random01()*3)+(down>4);
            for(int k=0;k<crown;++k) { float a=range(0,6.2832f),s=range(.3f,.9f);
                spawn(nx,ny,floor+.5,std::cos(a)*s,std::sin(a)*s,range(.7f,1.5f),range(.09f,.16f),40,0,p.fluid); }
            // A drop into a puddle is a faint plip; most go unheard under
            // the game, so only one in about half a second is voiced.
            if(dripSound+18<=leveltime) { dripSound=leveltime; Native_GoreSound(gore_sound::Drip,nx,ny,floor,.25); }
            p.active=false; return;
        }
        if(!p.chunk) {
            if(!mist) {
                // A shallow impact skids into a teardrop along its travel and
                // throws satellite beads ahead; a vertical one stays round.
                double stretch=std::clamp(1+.55*across/std::max(down,.6),1.0,2.8),radius=1.0+p.size*(3.0+std::min(impact,6.0)*.8);
                double ux=across>1e-4?p.vx/across:0,uy=across>1e-4?p.vy/across:0,shift=radius*(stretch-1)*.5;
                addPlane(nx+ux*shift,ny+uy*shift,sector,radius,false,p.fluid,false,0,ux,uy,stretch);
                if(p.size>.3f && across>2) for(int k=int(random01()*3);k>=0;--k) {
                    double ahead=radius*stretch*range(1.1f,2.2f),side=range(-1,1)*radius*.6;
                    double bx=nx+ux*ahead-uy*side,by=ny+uy*ahead+ux*side;
                    addPlane(bx,by,sectorAt(bx,by),radius*range(.18f,.32f),false,p.fluid,false,0,ux,uy,1.3);
                }
                // A heavy drop landing hard throws a small crown of droplets.
                if(p.size>.38f && down>3 && shared.activeParticles<ParticleCount*3/4) for(int k=0;k<3;++k) {
                    float a=range(0,6.2832f),s=range(.4f,1.1f);
                    spawn(nx,ny,floor+.6,std::cos(a)*s+p.vx*.15,std::sin(a)*s+p.vy*.15,range(.6f,1.3f),range(.08f,.14f),40,0,p.fluid);
                }
            }
            else if(random01()<.3f) addPlane(nx,ny,sector,.6+p.size*2,false,p.fluid,false,0);
            p.active=false; return;
        }
        nz=floor+p.size*.5;
        // Meat hitting the floor is heard; bone skitters silently.
        if(p.chunk!=Bone && std::fabs(p.vz)>1.0f && slapSound+2<=leveltime && (p.chunk!=Flesh || p.size>.9f)) {
            slapSound=leveltime;
            Native_GoreSound(gore_sound::Slap,nx,ny,floor,std::min(1.0,std::fabs(p.vz)/5.0)*(p.chunk==Head || p.chunk==Limb?1:.7));
        }
        // Restitution by tissue: bone is hard and skips, soft parts slap and stick.
        float bounce=p.chunk==Bone?.45f:p.chunk==Viscera?.12f:.3f,slide=p.chunk==Bone?.75f:p.chunk==Viscera?.35f:.6f;
        if(std::fabs(p.vz)>1.3f) { p.vz=-p.vz*bounce; p.vx*=slide; p.vy*=slide; if(p.chunk!=Bone) addPlane(nx,ny,sector,p.size*(p.chunk==Viscera?2.2:1.6),false,p.fluid,false,0); }
        else {
            p.resting=1; p.vx=p.vy=p.vz=0; p.life=uint16_t(std::min(p.age+35*150,60000));
            // A severed head or limb drains its open vessels where it lies:
            // a few hundred millilitres spreading into a pool over seconds.
            if(p.chunk==Head || p.chunk==Limb) addPlane(nx,ny,sector,p.size*(p.chunk==Head?6.5:5.0),true,p.fluid,false,int(range(160,260)));
            else addPlane(nx,ny,sector,p.size*2.6,false,p.fluid,false,0);
        }
    } else if(nz+p.size>=ceiling) {
        if(sectors[sector].ceilingpic==skyflatnum) { p.active=false; return; }
        if(!p.chunk) {
            // A ceiling hit spreads into a slowly widening stain and the
            // impact sheds a few droplets straight back down.
            if(!mist && p.vz>1.2f) {
                addPlane(nx,ny,sector,1.3*(1.0+p.size*(3+p.vz*.4)),false,p.fluid,true,int(range(50,110)));
                if(p.vz>2.5f) for(int k=0;k<2;++k) spawn(nx,ny,ceiling-1,p.vx*.1+range(-.3f,.3f),p.vy*.1+range(-.3f,.3f),-range(.2f,.6f),range(.1f,.18f),60,0,p.fluid);
            }
            p.active=false; return;
        }
        nz=ceiling-p.size; p.vz=-std::fabs(p.vz)*.2f;
    }
    p.z=float(nz);
    if(++p.age>=p.life) p.active=false;
}

void stepBleeders() {
    for(size_t i=0;i<bleeders.size();) {
        Bleeder &b=bleeders[i];
        const mobj_t *mo=b.mo; int age=leveltime-b.start;
        double x=mo->x*Unit,y=mo->y*Unit;
        uint8_t fluid=fluidOf(mo);
        bool grounded=mo->z<=mo->floorz+FRACUNIT;
        // The pool starts once the body has collapsed and spreads over
        // several seconds, slowing as it widens.
        if(b.pool<0 && grounded && age>=(b.gib?6:18)) {
            int sector=sectorAt(x,y);
            double radius=std::clamp(mo->radius*Unit*1.3*(b.gib?1.45:1),14.0,60.0);
            unsigned before=shared.planeCursor;
            addPlane(x,y,sector,radius,true,fluid,false,int(range(250,450)));
            // -2: merged into an existing stain, so this body owns no pool.
            b.pool=shared.planeCursor!=before?int((shared.planeCursor-1)%PlaneCount):-2;
            b.lastx=float(x); b.lasty=float(y);
        }
        // Arterial spurts: each heartbeat drives a gushing jet out of the
        // exit wound, along the killing hit and arcing up like a fountain. A
        // systolic stroke lasts about 0.1 s (four tics, peak first); pressure
        // falls with blood loss (~1.3 s time constant) while the heart slows,
        // so the jets shrink from ~2 m to a dribble. A gibbed body pumps from
        // four torn vessels at once, each beat aimed somewhere new.
        int stroke=leveltime-b.beat;
        if(age<(b.gib?90:130) && stroke>=0 && stroke<4) {
            static const float profile[4]={1,.85f,.55f,.3f};
            double pressure=std::exp(-age/45.0)*profile[stroke];
            double wound=mo->z*Unit+mo->height*Unit*std::max(.15,.55-age/140.0),r=mo->radius*Unit*.7;
            if(stroke==0 && pressure>.2 && spurtSound!=leveltime) { spurtSound=leveltime; Native_GoreSound(gore_sound::Spurt,x,y,wound,pressure); }
            for(int v=0;v<(b.gib?4:1);++v) {
                double a=b.gib?double(hash(uintptr_t(mo)+unsigned(b.beat*7+v))%6283)/1000:std::atan2(b.dy,b.dx)+std::sin(age*.21)*.18;
                double ux=std::cos(a),uy=std::sin(a),speed=1.8+5.2*pressure;
                int count=int(4+14*pressure);
                for(int k=0;k<count;++k) { float yaw=range(-.09f,.09f),s=float(speed)*range(.8f,1.08f);
                    double cx=ux*std::cos(yaw)-uy*std::sin(yaw),cy=ux*std::sin(yaw)+uy*std::cos(yaw);
                    spawn(x+ux*r,y+uy*r,wound,cx*s,cy*s,s*range(.35f,.7f),range(.22f,.48f),150,0,fluid); }
                if(pressure>.4) spray(x+ux*r,y+uy*r,wound,ux,uy,.5,int(3+5*pressure),.4f,.6f,2,.07f,.15f,int(range(12,22)),fluid);
            }
            if(stroke==3) b.beat=leveltime+11+int(hash(uintptr_t(mo))%4)+age/14;
        }
        // Between beats, and long after them, the wound keeps pouring in a
        // steady stream that thins as the body bleeds out (~3 s constant),
        // falling with the collapsing body; it feeds the pool below.
        else if(age<260) {
            double flow=.9*std::exp(-age/105.0),wound=mo->z*Unit+mo->height*Unit*std::max(.08,.55-age/140.0),r=mo->radius*Unit*.7;
            if(flow>.12) for(int v=0;v<(b.gib?2:1);++v) {
                double a=b.gib?double(hash(uintptr_t(mo)+unsigned(v)*977u)%6283)/1000:std::atan2(b.dy,b.dx);
                pour(x+std::cos(a)*r,y+std::sin(a)*r,wound,std::cos(a),std::sin(a),flow,fluid);
            }
        }
        // A sliding corpse leaves a smear.
        if(grounded && b.pool!=-1 && std::hypot(x-b.lastx,y-b.lasty)>6) {
            addPlane(x,y,sectorAt(x,y),mo->radius*Unit*.5,false,fluid,false,0);
            b.lastx=float(x); b.lasty=float(y);
        }
        if((age>140 && b.pool!=-1) || age>350) bleeders.erase(bleeders.begin()+long(i)); else ++i;
    }
}

// A side coordinate in map space and the unit normal into that side's sector.
void sidePoint(const line_t *ld,int side,double u,double &x,double &y,double &nx,double &ny) {
    double lx=ld->dx*Unit,ly=ld->dy*Unit,len=std::max(1e-6,std::hypot(lx,ly)),ux=lx/len,uy=ly/len;
    if(side) { x=ld->v2->x*Unit-ux*u; y=ld->v2->y*Unit-uy*u; nx=-uy; ny=ux; }
    else { x=ld->v1->x*Unit+ux*u; y=ld->v1->y*Unit+uy*u; nx=uy; ny=-ux; }
}
// Drips that reach the bottom of their wall tier: on a floor they leave a
// small stain at the wall base; off an upper tier's edge they fall as drops.
// Fresh ceiling stains shed a few drops that land directly below them.
void stepSurfaces() {
    for(int i=0;i<WallCount;++i) {
        WallDecal &d=shared.walls[i];
        if(!d.active || d.drip<=0 || d.line<0 || d.line>=numlines) continue;
        int count=rivulets(d);
        if(d.landed==(1<<count)-1) continue;
        const line_t *ld=&lines[d.line];
        if(ld->sidenum[d.side]<0) continue;
        const sector_t *own=sides[ld->sidenum[d.side]].sector;
        double base=d.anchor>=0?(d.anchorCeiling?ceilingOf(d.anchor):floorOf(d.anchor)):0;
        double bottom=d.anchorCeiling?base:own->floorheight*Unit,t=leveltime-d.born;
        for(int k=0;k<count;++k) {
            if(d.landed&(1<<k)) continue;
            Rivulet r=rivulet(d,i,k);
            if(base+d.z-d.radius*.2-rivuletRun(r,t)>bottom+.25) {
                if(t>r.delay+r.tau*4) d.landed|=uint8_t(1<<k); // dried before reaching it
                continue;
            }
            d.landed|=uint8_t(1<<k);
            double x,y,nx,ny; sidePoint(ld,d.side,d.u+r.offset,x,y,nx,ny);
            if(d.anchorCeiling) spawn(x+nx*1.2,y+ny*1.2,bottom-.6,0,0,-.2,range(.22f,.32f),300,0,d.fluid);
            else addPlane(x+nx*1.6,y+ny*1.6,int(own-sectors),1.2+r.width*1.4,false,d.fluid,false,0,nx,ny,1.5);
        }
    }
    for(size_t i=0;i<ceilingDrips.size();) {
        CeilingDrip &c=ceilingDrips[i];
        const PlaneDecal &d=shared.planes[c.index];
        if(!d.active || !d.ceiling || d.born!=c.born || c.left<=0) { c=ceilingDrips.back(); ceilingDrips.pop_back(); continue; }
        if(leveltime>=c.next) {
            float a=range(0,6.2832f),r=d.radius*.45f*random01();
            // Steady drips: each bead gathers on the ceiling, falls, lands
            // as a plop and builds a puddle; the run slows as the stain dries.
            if(Particle *p=spawn(d.x+std::cos(a)*r,d.y+std::sin(a)*r,ceilingOf(d.sector)-.6,0,0,0,.08f,200,0,d.fluid)) {
                p->spread=2; p->resting=2; p->opacity=uint8_t(range(22,40));
            }
            --c.left; c.next=leveltime+int(range(30,55)*(1+(leveltime-c.born)/900.0));
        }
        ++i;
    }
}

// Bloody footprints (2026-10-10). A foot that lands in wet blood picks
// some up: a fresh pool soaks the sole, a splat wets part of it, an older
// print barely at all, and blood that has dried no longer transfers. Each
// later step prints part of what that foot still carries, so a trail fades
// over ten to fifteen steps from a full sole to the lugs and heel edge that
// press hardest. Blood left on a sole also dries (about a minute), so a
// trail stops even without walking it off. Every walking body leaves its own
// prints from its own gait: step length follows speed and leg length, the
// feet land either side of the line of travel and turn slightly outward.
struct Foot { int kind; double length,width,scale; };
bool footOf(const mobj_t *m,Foot &f) {
    switch(m->type) {
    case MT_PLAYER: case MT_POSSESSED: case MT_SHOTGUY: case MT_CHAINGUY: case MT_WOLFSS: f={PrintBoot,9.5,3.6,1}; return true;
    case MT_TROOP: f={PrintClaw,9,4.5,1}; return true;
    case MT_SERGEANT: case MT_SHADOWS: f={PrintClaw,11,6.5,1.1}; return true;
    case MT_UNDEAD: case MT_VILE: f={PrintClaw,9.5,4,1.15}; return true;
    case MT_KNIGHT: case MT_BRUISER: f={PrintHoof,8,6.5,1.15}; return true;
    case MT_FATSO: f={PrintPad,15,13,1.2}; return true;
    case MT_BABY: f={PrintPad,10,10,1}; return true;
    case MT_SPIDER: f={PrintPad,22,20,1.8}; return true;
    case MT_CYBORG: f={PrintHoof,17,13,1.9}; return true;
    default: return false;
    }
}
constexpr double UnitsPerMeter=32,TicsPerSecond=35;
// Step length of a human gait at v m/s: cadence rises from 1.8 steps/s at a
// walk to about 4.6 in a sprint, and no stride exceeds 2.4 m. Longer legs
// scale the whole gait.
double stepLength(double unitsPerTic,double scale) {
    double v=unitsPerTic*TicsPerSecond/UnitsPerMeter/scale,cadence=std::clamp(1.8+.32*v,1.8,4.6);
    return std::clamp(v/cadence,.45,2.4)*UnitsPerMeter*scale;
}
struct Walker { double x=0,y=0,travelled=0,speed=0; float load[2]={0,0}; uint8_t fluid[2]={Red,Red},foot=0; mobjtype_t type{}; bool seen=false; int standing=0; };
std::unordered_map<const mobj_t *,Walker> walkers;
const float soleDrying=float(std::exp(-1/(TicsPerSecond*60)));
double grownRadius(const PlaneDecal &d) {
    if(!d.grow) return d.radius;
    return d.radius*std::max(.08,1-std::exp(-3.0*std::max(0,leveltime-d.born)/d.grow));
}
// Pools that reach a step down run over its edge: the overflow streams
// down the riser in rivulets, a few drops fall clear and a smaller pool
// spreads on the floor below, which may spill again, so blood cascades
// down a staircase losing volume at each step. Checked every 7 tics,
// staggered across pools.
void spillPools() {
    for(int i=0;i<PlaneCount;++i) {
        PlaneDecal &d=shared.planes[i];
        if(!d.active || !(d.shape&PoolBit) || (d.shape&PrintBit) || d.ceiling || d.spilled || !d.grow || (leveltime+i)%7) continue;
        double R=grownRadius(d);
        if(R<4) continue;
        double top=floorOf(d.sector),best=R*1.06+1.5; int line=-1,side=0; double along=0;
        linesNear(d.x-best,d.y-best,d.x+best,d.y+best,[&](line_t *ld,int) {
            if(!(ld->flags&ML_TWOSIDED) || !ld->frontsector || !ld->backsector) return;
            int own=ld->frontsector==&sectors[d.sector]?0:ld->backsector==&sectors[d.sector]?1:-1;
            if(own<0) return;
            const sector_t *low=own?ld->frontsector:ld->backsector;
            double drop=top-low->floorheight*Unit;
            if(drop<2 || drop>256 || liquidFloor(int(low-sectors)) || low->ceilingheight*Unit<=top) return;
            double ax=ld->v1->x*Unit,ay=ld->v1->y*Unit,lx=ld->dx*Unit,ly=ld->dy*Unit,len=lx*lx+ly*ly;
            double t=len>0?std::clamp(((d.x-ax)*lx+(d.y-ay)*ly)/len,0.0,1.0):0,dist=std::hypot(d.x-ax-t*lx,d.y-ay-t*ly);
            if(dist<best) { best=dist; line=int(ld-lines); side=own^1; along=t; }
        });
        if(line<0) continue;
        d.spilled=1;
        const line_t *ld=&lines[line];
        int lower=int(sides[ld->sidenum[side]].sector-sectors);
        double length=lineLength(ld),u=(side?1-along:along)*length,x,y,nx,ny;
        sidePoint(ld,side,u,x,y,nx,ny);
        Hit edge; edge.t=0; edge.line=line; edge.side=side; edge.s=along; edge.z=top-.5;
        addWall(edge,std::min(R*.35,10.0),1,d.fluid,std::min(8+R*.4,20.0));
        for(int k=0;k<3;++k) spawn(x+nx*.8+range(-2,2)*ny,y+ny*.8-range(-2,2)*nx,top-.3,nx*range(.2f,.6f),ny*range(.2f,.6f),0,range(.25f,.4f),200,0,d.fluid);
        double reach=std::min(R*.5,12.0);
        addPlane(x+nx*reach,y+ny*reach,lower,R*.7,true,d.fluid,false,int(d.grow*1.2));
    }
}
// Blood a foot at (x,y) can pick up from the floor: thickness times how wet
// it still is (thin films dry in ~20 s, thick pools stay wet over a minute).
double wetBlood(double x,double y,int sector,double footRadius,uint8_t &fluid) {
    double best=0;
    for(const PlaneDecal &d:shared.planes) {
        if(!d.active || d.ceiling || d.sector!=sector) continue;
        double radius=grownRadius(d),dx=x-d.x,dy=y-d.y,reach=radius*d.stretch+footRadius;
        if(dx*dx+dy*dy>reach*reach) continue;
        double along=(dx*d.c+dy*d.s)/(radius*d.stretch*.85+footRadius),across=(dy*d.c-dx*d.s)/(radius*.85+footRadius);
        if(along*along+across*across>1) continue;
        double thickness=(d.shape&PoolBit)?1.0:(d.shape&PrintBit)?.12*d.load/255:.35*std::min(1.0,radius/5);
        double density=(d.shape&PoolBit)?200:(d.shape&PrintBit)?40:100,dry=TicsPerSecond*(20+70*density/255);
        double amount=thickness*(1-(leveltime-d.born)/dry);
        if(amount>best) { best=amount; fluid=d.fluid; }
    }
    return best;
}
void addPrint(double x,double y,int sector,double c,double s,const Foot &f,bool left,double load,uint8_t fluid) {
    double length=f.length*f.scale,width=f.width*f.scale;
    if(sector<0 || liquidFloor(sector) || clearance(x,y,length*.5,false)<width*.5) return; // never across a wall or a step edge
    PlaneDecal &d=shared.planes[shared.planeCursor++%PlaneCount];
    if(d.active) { auto found=layers.find(layerKey(d.x,d.y,d.ceiling)); if(found!=layers.end() && !--found->second) layers.erase(found); }
    ++layers[layerKey(x,y,false)];
    d={float(x),float(y),float(width*.5),float(c),float(s),float(length/width),sector,leveltime,0,
       uint8_t(PrintBit|(left?LeftBit:0)|f.kind),fluid,0,uint8_t(random01()*3),true};
    d.load=uint8_t(std::clamp(load,0.0,1.0)*255);
}
void stepWalkers() {
    for(auto &entry:walkers) entry.second.seen=false;
    for(thinker_t *th=thinkercap.next;th!=&thinkercap;th=th->next) {
        if(th->function.acp1!=(actionf_p1)P_MobjThinker) continue;
        auto *m=reinterpret_cast<mobj_t *>(th);
        Foot f;
        if(m->health<=0 || (m->flags&(MF_NOGRAVITY|MF_FLOAT|MF_CORPSE)) || !footOf(m,f)) continue;
        double x=m->x*Unit,y=m->y*Unit;
        Walker &w=walkers[m];
        if(w.type!=m->type) { w=Walker{}; w.x=x; w.y=y; w.type=m->type; }
        w.seen=true;
        double dx=x-w.x,dy=y-w.y,moved=std::hypot(dx,dy);
        w.x=x; w.y=y;
        for(float &l:w.load) l*=soleDrying;
        if(moved>64) { w.travelled=0; continue; } // teleported
        w.speed+=(moved-w.speed)*.2;
        if(m->z>m->floorz+FRACUNIT) continue;
        if(liquidFloor(sectorAt(x,y))) { w.load[0]=w.load[1]=0; w.travelled=0; continue; }
        if(moved<1e-3) {
            // Standing in blood (a pool spreading under the feet) wets both
            // soles; checked every quarter second while the body stays put.
            if(++w.standing%9) continue;
            int sector=sectorAt(x,y); uint8_t fluid=Red;
            double wet=sector<0?0:wetBlood(x,y,sector,f.width*f.scale*.5+(.055*UnitsPerMeter*f.scale),fluid);
            if(wet>0) for(int k=0;k<2;++k) { if(wet>w.load[k]*.5) w.fluid[k]=fluid; w.load[k]=float(w.load[k]+(1-w.load[k])*wet); }
            continue;
        }
        w.standing=0;
        w.travelled+=moved;
        double step=stepLength(w.speed,f.scale);
        if(w.travelled<step) continue;
        w.travelled=std::fmod(w.travelled-step,step);
        w.foot^=1;
        bool left=w.foot==1;
        double c=dx/moved,s=dy/moved,v=w.speed*TicsPerSecond/UnitsPerMeter/f.scale;
        // Feet land ~5 cm either side of the line of travel at a walk and
        // close toward it when running; toes turn out about 7 degrees.
        double lateral=(left?1:-1)*(.055-.035*std::min(v/6,1.0))*UnitsPerMeter*f.scale*(f.width/3.6);
        double fx=x-s*lateral,fy=y+c*lateral,turn=left?.12:-.12;
        double pc=c*std::cos(turn)-s*std::sin(turn),ps=s*std::cos(turn)+c*std::sin(turn);
        int sector=sectorAt(fx,fy);
        if(sector<0) continue;
        int foot=w.foot; float &load=w.load[foot];
        uint8_t fluid=w.fluid[foot];
        double wet=wetBlood(fx,fy,sector,f.width*f.scale*.5,fluid);
        if(wet>0) {
            if(wet>load*.5) w.fluid[foot]=fluid;
            load=float(load+(1-load)*wet);
        }
        // A foot planted in the pool itself leaves no separate print.
        if(wet<.5 && load>.035f) { addPrint(fx,fy,sector,pc,ps,f,left,load,w.fluid[foot]); load*=.78f; }
    }
    for(auto it=walkers.begin();it!=walkers.end();) if(it->second.seen) ++it; else it=walkers.erase(it);
}

}

State &state() { return shared; }
bool enabled() { return Native_Enhanced(); }
uint8_t shade(uint8_t fluid,int density,int age,int tone) {
    // Real blood is a saturated red with almost no green or blue: a fresh
    // oxygenated film about (140,3,3), a thick pool (90-100,0,0). It darkens
    // as it deoxygenates and clots, and a thin film dries to a brown crust
    // (about 50,25,15). Red uses the palette's pure-red ramp 184-191
    // (155..67,0,0) and the browns 238-239. Blue 197-207/240-243 and green
    // 114-127 are the fantasy fluids.
    static const uint8_t red[]={184,185,186,187,188,189,190,191};
    static const uint8_t blue[]={197,198,199,200,201,202,203,204,205,206,207,240,241,242,243};
    static const uint8_t green[]={114,115,116,117,118,119,120,121,122,123,124,125,126,127,127};
    double film=std::clamp(density,0,255)/255.0;
    // Thin films dry within ~20 s; a thick pool stays wet for over a minute.
    // Drying is continuous: one shade step at a time, never a jump.
    double dryTics=35*(20+70*film),dry=std::clamp(age/dryTics,0.0,1.5);
    if(fluid==Red) {
        if(film<.36 && age>dryTics*2) return tone?239:238;
        // A thick pool ends as a near-black crust (hemichrome) much later.
        if(age>dryTics*3) return 239;
        double at=.4*tone+film*4+dry*2.4;
        return red[std::clamp(int(at),0,7)];
    }
    const uint8_t *ramp=fluid==Blue?blue:green;
    return ramp[std::clamp(int(tone+film*7+dry*4),0,14)];
}
const uint8_t *shadeTable(uint8_t fluid,int age,int tone) {
    // Read by parallel render workers: every table is built once up front.
    constexpr int Seconds=320;
    static uint8_t tables[3][3][Seconds][16];
    static std::once_flag once;
    std::call_once(once,[] {
        for(int f=0;f<3;++f) for(int t=0;t<3;++t) for(int second=0;second<Seconds;++second)
            for(int level=0;level<16;++level) tables[f][t][second][level]=shade(uint8_t(f),level*16+8,second*35+17,t);
    });
    return tables[std::min<int>(fluid,2)][std::clamp(tone,0,2)][std::clamp(age/35,0,Seconds-1)];
}
uint8_t sheen(uint8_t color) {
    if(color>=185 && color<=191) return uint8_t(color-1);
    if(color>=199 && color<=207) return uint8_t(color-2);
    if(color>=117 && color<=127) return uint8_t(color-2);
    return color;
}
int rivulets(const WallDecal &d) { return d.drip<=0?0:1+(d.drip>6)+(d.drip>10)+(d.drip>14); }
Rivulet rivulet(const WallDecal &d,int index,int k) {
    uint32_t h=hash(hash(uint32_t(index)*977u+uint32_t(d.born))+uint32_t(k)),g=hash(h);
    Rivulet r;
    r.offset=((h&255)/255.0-.5)*d.radius*.9;
    r.width=.3+((h>>8)&255)/255.0*.35;
    r.length=d.drip*(.45+((h>>16)&255)/255.0*.55);
    r.delay=6+((h>>24)&15)*1.5;
    r.tau=25+(g&63);
    return r;
}
double rivuletRun(const Rivulet &r,double tics) { return tics<=r.delay?0:r.length*(1-std::exp(-(tics-r.delay)/r.tau)); }
}

using namespace gore;
void Native_GoreReset(void) {
    for(auto &p:shared.particles) p.active=false;
    for(auto &d:shared.planes) d.active=false;
    for(auto &d:shared.walls) d.active=false;
    shared.sideHead.clear(); shared.particleCursor=shared.planeCursor=shared.wallCursor=0;
    shared.simTime=-1; bleeders.clear(); wounds.clear(); spurtSound=slapSound=dripSound=-1; pending={}; budget={}; layers.clear(); ceilingDrips.clear(); walkers.clear();
}
void Native_GoreShot(void *target,int32_t x,int32_t y,int32_t z,int32_t dx,int32_t dy,int32_t slope,int damage,int corpse) {
    if(!enabled() || !target) return;
    auto *t=static_cast<const mobj_t *>(target);
    double h=std::hypot(double(dx),double(dy));
    if(h<1) return;
    Pending shot{t,leveltime,x*Unit,y*Unit,z*Unit,dx/h,dy/h,slope*Unit};
    if(corpse) { ensureLevel(); impact(t,shot.x,shot.y,shot.z,shot.dx,shot.dy,shot.dz,damage,true,false); }
    else pending=shot;
}
void Native_GoreHit(void *targetPointer,void *sourcePointer,void *inflictorPointer,int damage) {
    if(!enabled() || damage<=0 || gamestate!=GS_LEVEL) return;
    const auto *t=static_cast<const mobj_t *>(targetPointer);
    const auto *source=static_cast<const mobj_t *>(sourcePointer);
    const auto *inflictor=static_cast<const mobj_t *>(inflictorPointer);
    if(!t || (t->flags&MF_NOBLOOD) || t->health<=0) return;
    ensureLevel();
    double cx=t->x*Unit,cy=t->y*Unit,mid=t->z*Unit+t->height*Unit*.6;
    double x=cx,y=cy,z=mid,dx=1,dy=0,dz=0; bool melee=false,blast=false,crush=false;
    if(pending.target==t && pending.tic==leveltime) {
        x=pending.x; y=pending.y; z=pending.z; dx=pending.dx; dy=pending.dy; dz=pending.dz; pending={};
        melee=source && P_AproxDistance(source->x-t->x,source->y-t->y)<MELEERANGE+t->radius;
    } else if(inflictor && (inflictor->flags&MF_MISSILE)) {
        dx=inflictor->momx*Unit; dy=inflictor->momy*Unit; dz=inflictor->momz*Unit;
        if(std::hypot(dx,dy)<1e-3) { dx=cx-inflictor->x*Unit; dy=cy-inflictor->y*Unit; }
        z=std::clamp(inflictor->z*Unit+inflictor->height*Unit*.5,t->z*Unit+4,t->z*Unit+t->height*Unit-2);
        double h=std::hypot(dx,dy); x=cx-dx/h*t->radius*Unit; y=cy-dy/h*t->radius*Unit;
    } else if(inflictor && inflictor!=source) {
        blast=true; dx=cx-inflictor->x*Unit; dy=cy-inflictor->y*Unit; dz=.5;
    } else if(source) {
        melee=true; dx=cx-source->x*Unit; dy=cy-source->y*Unit;
        double h=std::hypot(dx,dy); if(h>0) { x=cx-dx/h*t->radius*Unit; y=cy-dy/h*t->radius*Unit; }
    } else crush=true;
    if(std::hypot(dx,dy)<1e-3) { float a=range(0,6.2832f); dx=std::cos(a); dy=std::sin(a); }
    int after=t->health-damage;
    bool death=after<=0,gib=death && ((after< -t->info->spawnhealth && t->info->xdeathstate) || damage>=1000);
    uint8_t fluid=fluidOf(t);
    if(crush) {
        int count=allow(t,26);
        for(int i=0;i<count;++i) { float a=range(0,6.2832f),s=range(1.5f,5);
            spawn(cx+std::cos(a)*t->radius*Unit*.6,cy+std::sin(a)*t->radius*Unit*.6,t->z*Unit+range(1,10),std::cos(a)*s,std::sin(a)*s,range(.3f,2),range(.2f,.5f),150,0,fluid); }
        addPlane(cx,cy,sectorAt(cx,cy),t->radius*Unit*1.1,true,fluid,false,25);
    } else if(blast) {
        double h=std::hypot(dx,dy); dx/=h; dy/=h;
        double k=std::clamp(damage/10.0,.5,9.0);
        spray(cx,cy,mid,dx,dy,.35,allow(t,int(std::min(110.0,14+9*k))),.9f,2.5f,9,.15f,.55f,160,fluid,3);
        spray(cx,cy,mid,dx,dy,.2,allow(t,int(10+4*k)),1.4f,.6f,2.8f,.08f,.2f,int(range(12,24)),fluid);
        if(!t->player) impact(t,cx-dx*t->radius*Unit,cy-dy*t->radius*Unit,mid,dx,dy,.2,damage/2,false,false);
    } else impact(t,x,y,z,dx,dy,dz,death && !t->player?int(damage*2+20):damage,false,melee);
    if(!death && !crush) addWound(t,z,dx,dy,damage);
    // Head hits (top tenth) burst outward: a dense mist cloud and tissue.
    if(!crush && !blast && !t->player && z>t->z*Unit+t->height*Unit*.88) {
        spray(cx,cy,z,dx,dy,.15,allow(t,40),1.2f,1.5f,5,.07f,.16f,int(range(16,30)),fluid,.6f);
        int pieces=allow(t,death?5:2);
        for(int i=0;i<pieces;++i) { float a=float(std::atan2(dy,dx))+range(-.6f,.6f),v=range(2,5);
            spawn(cx,cy,z,std::cos(a)*v,std::sin(a)*v,range(1,3),range(.4f,.8f),400,Flesh,fluid); }
    }
    if(death) {
        if(gib) gibs(t,fluid,t->player?1:std::clamp(t->info->spawnhealth/60.0,1.0,2.2));
        double h=std::hypot(dx,dy);
        addBleeder(t,gib,dx/h,dy/h);
    }
}
void Native_GoreCrush(void *thing) {
    const auto *t=static_cast<const mobj_t *>(thing);
    if(!enabled() || !t || (t->flags&MF_NOBLOOD) || gamestate!=GS_LEVEL) return;
    ensureLevel();
    gibs(t,fluidOf(t),1.2);
    double x=t->x*Unit,y=t->y*Unit;
    addPlane(x,y,sectorAt(x,y),std::clamp(t->info->radius*Unit*1.5,20.0,64.0),true,fluidOf(t),false,20);
}
void Native_GoreDespawn(void *thing) {
    if(pending.target==thing) pending={};
    if(budget.target==thing) budget={};
    bleeders.erase(std::remove_if(bleeders.begin(),bleeders.end(),[&](const Bleeder &b) { return b.mo==thing; }),bleeders.end());
    wounds.erase(std::remove_if(wounds.begin(),wounds.end(),[&](const Wound &w) { return w.mo==thing; }),wounds.end());
    walkers.erase(static_cast<const mobj_t *>(thing));
}
void Native_GoreTic(void) {
    if(!enabled() || gamestate!=GS_LEVEL || !sectors || leveltime==shared.simTime) return;
    shared.simTime=leveltime;
    ensureLevel();
    stepBleeders();
    stepWounds();
    stepSurfaces();
    spillPools();
    stepWalkers();
    unsigned particles=0,planes=0,walls=0,prints=0;
    for(auto &p:shared.particles) if(p.active) { stepParticle(p); particles+=p.active; }
    for(auto &d:shared.planes) { planes+=d.active; prints+=d.active && (d.shape&PrintBit); }
    for(auto &d:shared.walls) walls+=d.active;
    shared.activeParticles=particles; shared.activePlanes=planes; shared.activeWalls=walls; shared.activePrints=prints;
}

