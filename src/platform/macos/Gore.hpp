// SPDX-License-Identifier: GPL-2.0-only
// Shared cosmetic gore state for the simulation (Gore.cpp) and the renderer
// (GoreRender.cpp). Nothing here is part of the game state or saved games.
#pragma once
#include <cstdint>
#include <vector>
#include "GoreMasks.hpp"

namespace gore {
enum Fluid : uint8_t { Red, Blue, Green };
// Particle.chunk: 0 is a blood drop; the rest are solid pieces of a body.
enum Piece : uint8_t { Drop, Flesh, Bone, Limb, Head, Viscera };

struct Particle {
    float x=0,y=0,z=0,px=0,py=0,pz=0,vx=0,vy=0,vz=0,size=1;
    int32_t sector=-1;
    uint16_t age=0,life=0;
    uint8_t chunk=0,fluid=Red,tone=0,resting=0;
    uint8_t opacity=0,spread=0; // spread: 1 stream drop, 2 ceiling bead; opacity: a bead's hang time in tics
    bool active=false;
};
// Floor/ceiling stain anchored to its sector's moving plane. (c,s) is the
// major axis; stretch elongates oblique impacts along the drop's travel.
// shape: bit 7 pool, bit 6 footprint (bit 5 left foot, low bits PrintKind),
// otherwise a splat. load: blood the sole still carried for a footprint.
constexpr uint8_t PoolBit=0x80, PrintBit=0x40, LeftBit=0x20;
struct PlaneDecal {
    float x=0,y=0,radius=0,c=1,s=0,stretch=1;
    int32_t sector=-1,born=0;
    uint16_t grow=0;
    uint8_t shape=0,fluid=Red,ceiling=0,tone=0;
    bool active=false;
    uint8_t load=255;
    uint8_t spilled=0; // a pool that has already run over a step edge
};
// Wall stain in side space: u from the side's start vertex, z relative to
// the plane that moves the texture tier (door ceilings, lift floors).
struct WallDecal {
    float u=0,z=0,radius=0,stretch=1,drip=0;
    int32_t line=-1,anchor=-1,born=0,next=-1;
    uint8_t side=0,shape=0,fluid=Red,tone=0,anchorCeiling=0,landed=0;
    bool active=false;
};
// One drip running down from a wall stain: it starts after the stain has
// pooled, then slows as its film thins. Shared by simulation and renderer so
// a drip meets the floor in the frame it is drawn reaching it.
struct Rivulet { double offset=0,width=0,length=0,delay=0,tau=1; };
int rivulets(const WallDecal &d);
Rivulet rivulet(const WallDecal &d,int index,int k);
double rivuletRun(const Rivulet &r,double tics);

constexpr int ParticleCount=6144, PlaneCount=3072, WallCount=2560;

struct State {
    std::vector<Particle> particles=std::vector<Particle>(ParticleCount);
    std::vector<PlaneDecal> planes=std::vector<PlaneDecal>(PlaneCount);
    std::vector<WallDecal> walls=std::vector<WallDecal>(WallCount);
    std::vector<int32_t> sideHead; // two entries per linedef, -1 when empty
    unsigned particleCursor=0,planeCursor=0,wallCursor=0;
    int simTime=-1;
    unsigned activeParticles=0,activePlanes=0,activeWalls=0,activePrints=0;
};
State &state();
bool enabled();
// Palette index for a fluid at a thickness and age (tics), before lighting.
uint8_t shade(uint8_t fluid,int density,int age,int tone);
// Cached shade() for density>>4 (0..15) at the age's one-second bucket.
const uint8_t *shadeTable(uint8_t fluid,int age,int tone);
// The same blood one step lighter: specular sheen on a fresh wet surface.
uint8_t sheen(uint8_t color);
}
