// SPDX-License-Identifier: GPL-2.0-only
// Gore presentation inside the original software renderer. Particles become
// clipped vissprites; stains are written only into pixels where their floor,
// ceiling or wall tier was actually drawn, lit by that surface's own light.
#include "Native.h"
#include "Parallel.hpp"
#include "Gore.hpp"
extern "C" {
#include "doomstat.h"
#include "p_local.h"
#include "r_local.h"
#include "w_wad.h"
#include "z_zone.h"
extern visplane_t visplanes[];
extern visplane_t *lastvisplane;
extern lighttable_t *ds_colormap_next;
extern unsigned ds_lightweight;
extern byte *ylookup[];
extern int columnofs[];
}
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

using namespace gore;
namespace {
constexpr double Unit=1.0/FRACUNIT;
std::vector<int32_t> sectorHead,particleNext;
std::vector<native_particle_t> projected;
std::vector<double> rayX,rayY;
double renderTime=0;
bool frameReady=false;
// Wall stains prepared once per view for each side that is drawn: what every
// column of the side needs, without rehashing drips per column. Stored in the
// order their pixels win (the oldest stain on a side is drawn over newer ones),
// so a column can stop at the first stain that covers a pixel.
struct PreparedRun { double offset,width,length; };
struct PreparedWall {
    const uint8_t *mask,*tones;
    double u,halfWidth,radius,center,start,uLo,uHi;
    uint32_t index;
    int runs,shape;
    PreparedRun run[4];
};
struct PreparedSide { unsigned view=0; int begin=0,count=0; };
std::vector<PreparedSide> preparedSides;
std::vector<PreparedWall> preparedWalls;
unsigned preparedView=0;

// 4x4 ordered dither (-7..8): a density lands on one of two neighbouring
// shades in a fixed screen pattern, so thickness and drying read as smooth
// gradients and edges fade instead of stepping. Stable from frame to frame.
int dither(int x,int y) {
    static constexpr int8_t bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    return bayer[y&3][x&3]-7;
}
// Mask density between texels (bilinear), so stains have no blocky edges.
int sampleMask(const uint8_t *mask,double lx,double ly) {
    double fx=std::clamp(lx*MaskSize-.5,0.0,MaskSize-1.001),fy=std::clamp(ly*MaskSize-.5,0.0,MaskSize-1.001);
    int ix=int(fx),iy=int(fy),jx=std::min(ix+1,MaskSize-1),jy=std::min(iy+1,MaskSize-1); double ax=fx-ix,ay=fy-iy;
    double top=mask[iy*MaskSize+ix]*(1-ax)+mask[iy*MaskSize+jx]*ax,bottom=mask[jy*MaskSize+ix]*(1-ax)+mask[jy*MaskSize+jx]*ax;
    return int(top*(1-ay)+bottom*ay+.5);
}
void writePixel(int x,int y,uint8_t color,const lighttable_t *map,const lighttable_t *next,unsigned weight,
                uint16_t *light,const byte *base) {
    byte *dest=ylookup[y]+columnofs[x];
    *dest=map[color];
    if(light) light[dest-base]=(weight>>1)&&next?uint16_t(next[color]|((weight>>1)<<8)):0;
}
// Wet blood reflection (2026-10-09): screen-space, not ray traced. A fresh
// pool mirrors what is already drawn above the floor in its column (the
// wall and lights behind it), mixed by Schlick's Fresnel term: almost none
// looking straight down, strong at grazing angles. Reflections in blood are
// tinted by it and darkened. Mixes go through palette tables built once.
constexpr int Tiers=5;
const float tierWeight[Tiers]={.08f,.16f,.26f,.38f,.50f};
std::vector<uint8_t> mirror; // [tier][blood][reflected]
bool mirrorReady=false;
void buildMirror() {
    mirrorReady=true;
    int lump=W_CheckNumForName((char *)"PLAYPAL"); if(lump<0) return;
    const uint8_t *pal=static_cast<const uint8_t *>(W_CacheLumpNum(lump,PU_STATIC));
    // Nearest palette entry for a 32^3 RGB cube, then every mix is a lookup.
    std::vector<uint8_t> cube(32*32*32);
    parallel::forEach(32,[&](size_t r) {
        for(int g=0;g<32;++g) for(int b=0;b<32;++b) {
            int R=int(r)*8+4,G=g*8+4,B=b*8+4,best=0,bestDistance=1<<30;
            for(int i=0;i<256;++i) { int dr=pal[i*3]-R,dg=pal[i*3+1]-G,db=pal[i*3+2]-B,d=2*dr*dr+4*dg*dg+3*db*db; if(d<bestDistance) { bestDistance=d; best=i; } }
            cube[(r*32+g)*32+b]=uint8_t(best);
        }
    });
    mirror.resize(size_t(Tiers)*65536);
    parallel::forEach(size_t(Tiers)*256,[&](size_t row) {
        int tier=int(row/256),blood=int(row%256); float w=tierWeight[tier];
        for(int ref=0;ref<256;++ref) {
            // Light reflected off a dark red liquid: red passes, green/blue are absorbed.
            float r=pal[blood*3]*(1-w)+pal[ref*3]*w*.95f,g=pal[blood*3+1]*(1-w)+pal[ref*3+1]*w*.45f,b=pal[blood*3+2]*(1-w)+pal[ref*3+2]*w*.40f;
            auto q=[](float v) { return std::clamp(int(v)>>3,0,31); };
            mirror[row*256+size_t(ref)]=cube[(q(r)*32+q(g))*32+q(b)];
        }
    });
}

double growth(int born,int grow) {
    if(!grow) return 1;
    double t=std::max(0.0,renderTime-born);
    return std::max(.08,1-std::exp(-3*t/grow));
}
double anchorHeight(const WallDecal &d) {
    if(d.anchor<0 || d.anchor>=numsectors) return 0;
    const auto *s=static_cast<const sector_t *>(Native_RenderSector(&sectors[d.anchor]));
    return (d.anchorCeiling?s->ceilingheight:s->floorheight)*Unit;
}
}

void Native_GoreFrame(void) {
    frameReady=false;
    if(!enabled() || gamestate!=GS_LEVEL || !sectors) return;
    State &g=state();
    renderTime=Native_RenderLevelTime();
    if(!mirrorReady) buildMirror();
    if(++preparedView==0) { for(auto &side:preparedSides) side.view=0; preparedView=1; }
    preparedWalls.clear(); preparedWalls.reserve(WallCount);
    double alpha=std::clamp(renderTime-(g.simTime-1),0.0,1.0);
    sectorHead.assign(numsectors,-1); particleNext.assign(ParticleCount,-1); projected.resize(ParticleCount);
    for(int i=0;i<ParticleCount;++i) {
        const Particle &p=g.particles[i];
        if(!p.active || p.sector<0 || p.sector>=numsectors) continue;
        double x=p.px+(p.x-p.px)*alpha,y=p.py+(p.y-p.py)*alpha,z=p.pz+(p.z-p.pz)*alpha;
        // Fast drops read as short streaks above their leading edge; a
        // stream's drops reach back to the one before, an unbroken column.
        double streak=p.chunk || p.resting?0:p.spread?std::min(std::max(0.0,-double(p.vz))*1.15+.9,14.0):std::min(std::max(0.0,-double(p.vz))*.7,6.0);
        bool mist=!p.chunk && p.life<40;
        int fade=mist?std::min(3,p.age/5):0;
        // Airborne drops are thin and backlit, so they read lighter than stains.
        // Pieces by tissue (PLAYPAL ramps): bone off-white, skin of severed
        // parts a dark flesh tone, viscera deep wet red, raw tissue red.
        uint8_t color;
        switch(p.chunk) {
        // A stream is a thick body of blood: deeper crimson, with a wet
        // highlight on every other drop so it glints even from afar.
        case Drop: color=p.spread?(i&1?sheen(shade(p.fluid,130,0,0)):shade(p.fluid,130,0,0)):shade(p.fluid,mist?20:60,0,p.tone+fade); break;
        case Bone: color=uint8_t(84+p.tone); break;
        case Head: case Limb: color=p.fluid==Red?uint8_t(64+p.tone*2):shade(p.fluid,220,0,p.tone); break;
        case Viscera: color=p.fluid==Red?uint8_t(38+p.tone*2):shade(p.fluid,230,0,p.tone); break;
        default: color=p.fluid==Red?uint8_t(34+p.tone*2):shade(p.fluid,200,0,p.tone); break;
        }
        projected[i]={int32_t(std::lround(x*FRACUNIT)),int32_t(std::lround(y*FRACUNIT)),int32_t(std::lround((z+streak)*FRACUNIT)),
                      int32_t(p.size*FRACUNIT*(mist?.8:1)),int32_t(streak*FRACUNIT),color,p.chunk};
        particleNext[i]=sectorHead[p.sector]; sectorHead[p.sector]=i;
    }
    rayX.resize(viewwidth); rayY.resize(viewwidth);
    double c=viewcos*Unit,s=viewsin*Unit,focal=projection*Unit;
    for(int x=0;x<viewwidth;++x) {
        if(native_render_height==200) {
            unsigned angle=(viewangle+xtoviewangle[x])>>ANGLETOFINESHIFT;
            double scale=distscale[x]*Unit;
            rayX[x]=finecosine[angle]*Unit*scale; rayY[x]=finesine[angle]*Unit*scale;
        } else {
            double offset=x-centerx+.5;
            rayX[x]=c+offset*s/focal; rayY[x]=s-offset*c/focal;
        }
    }
    frameReady=true;
}

void Native_GoreSprites(void *sector,void (*project)(const native_particle_t *)) {
    if(!frameReady) return;
    int index=Native_RenderSectorIndex(sector);
    if(index<0 || index>=int(sectorHead.size())) return;
    for(int i=sectorHead[index];i>=0;i=particleNext[i]) project(&projected[i]);
}

namespace {
// Per visplane row: light tables and the row's floor distance, valid for
// the frame that filled them. Each row belongs to exactly one band, so
// bands fill and read their rows without sharing.
struct RowLight { const lighttable_t *map=nullptr,*next=nullptr; unsigned weight=0,frame=0; double dist=0; };
std::vector<RowLight> rowPool;
std::vector<visplane_t *> framePlanes;
unsigned rowFrame=0;
// A stain that survived culling: its screen box and its first visplane.
struct Candidate { const PlaneDecal *d; double rx,ry,radius,reach; int x1,x2,y1,y2,first,age; bool wet; int threshold=0,film=255; };
std::vector<Candidate> candidates;
// Visible bounds of a stain's square after clipping it to the near plane.
bool screenBounds(double rx,double ry,double radius,double eye,double c,double s,double focal,int &x1,int &x2,int &y1,int &y2) {
    constexpr double Near=2;
    double px[4],pz[4]; int count=0;
    for(int corner:{0,1,3,2}) {
        double cx=rx+((corner&1)?radius:-radius),cy=ry+((corner&2)?radius:-radius);
        pz[count]=cx*c+cy*s; px[count]=cx*s-cy*c; ++count;
    }
    double minX=1e30,maxX=-1e30,minY=1e30,maxY=-1e30; bool any=false;
    auto add=[&](double tx,double tz) {
        double sx=centerx+tx*focal/tz,sy=centery+eye*focal/tz;
        minX=std::min(minX,sx); maxX=std::max(maxX,sx); minY=std::min(minY,sy); maxY=std::max(maxY,sy); any=true;
    };
    for(int i=0;i<4;++i) {
        int j=(i+1)%4; bool in=pz[i]>=Near,next=pz[j]>=Near;
        if(in) add(px[i],pz[i]);
        if(in!=next) { double t=(Near-pz[i])/(pz[j]-pz[i]); add(px[i]+(px[j]-px[i])*t,Near); }
    }
    if(!any) return false;
    x1=int(std::max(0.0,std::floor(minX)-1)); x2=int(std::min(viewwidth-1.0,std::ceil(maxX)+1));
    y1=int(std::max(0.0,std::floor(minY)-1)); y2=int(std::min(viewheight-1.0,std::ceil(maxY)+1));
    return x1<=x2 && y1<=y2;
}
}

void Native_GorePlanes(void) {
    if(!frameReady) return;
    State &g=state();
    if(!g.activePlanes) return;
    std::vector<visplane_t *> &planes=framePlanes; planes.clear();
    for(visplane_t *pl=visplanes;pl<lastvisplane;++pl) if(pl->minx<=pl->maxx && pl->picnum!=skyflatnum) planes.push_back(pl);
    if(planes.empty()) return;
    std::sort(planes.begin(),planes.end(),[](const visplane_t *a,const visplane_t *b) { return a->height<b->height; });
    size_t rows=planes.size()*size_t(viewheight);
    if(rowPool.size()<rows) rowPool.resize(rows);
    if(++rowFrame==0) { for(auto &r:rowPool) r.frame=0; rowFrame=1; }
    uint16_t *light=R_NativeLightPixels(); const byte *base=Native_RenderPixels();
    double vx=viewx*Unit,vy=viewy*Unit,c=viewcos*Unit,s=viewsin*Unit,focal=projection*Unit;
    // Serial culling; the shared masks and shade tables are built here.
    poolMask(0); shadeTable(Red,0,0);
    candidates.clear();
    for(const PlaneDecal &d:g.planes) {
        if(!d.active || d.sector<0 || d.sector>=numsectors) continue;
        const auto *sector=static_cast<const sector_t *>(Native_RenderSector(&sectors[d.sector]));
        fixed_t height=d.ceiling?sector->ceilingheight:sector->floorheight;
        if(d.ceiling?height<=viewz:height>=viewz) continue;
        double radius=d.radius*growth(d.born,d.grow),reach=radius*d.stretch;
        double rx=d.x-vx,ry=d.y-vy;
        if(rx*c+ry*s<-reach || rx*rx+ry*ry>4096.0*4096.0) continue;
        int x1,x2,y1,y2;
        if(!screenBounds(rx,ry,reach,(viewz-height)*Unit,c,s,focal,x1,x2,y1,y2)) continue;
        auto first=std::lower_bound(planes.begin(),planes.end(),height,[](const visplane_t *a,fixed_t h) { return a->height<h; });
        if(first==planes.end() || (*first)->height!=height) continue;
        int age=int(renderTime-d.born);
        // Fresh thick blood is glossy: at grazing view angles its surface
        // reflects, so the wet core reads a step lighter than the dry rim.
        candidates.push_back({&d,rx,ry,radius,reach,x1,x2,y1,y2,int(first-planes.begin()),age,!d.ceiling && age<35*45});
        if(d.shape&PrintBit) {
            // A footprint is a thin film: a loaded sole prints its whole
            // contact area, a nearly clean one only its hardest-pressed parts.
            double load=d.load/255.0;
            candidates.back().threshold=int(235*(1-load));
            candidates.back().film=int(70+140*load);
            candidates.back().wet=false;
        }
    }
    if(candidates.empty()) return;
    constexpr int Band=48;
    const int bands=(viewheight+Band-1)/Band;
    parallel::forEach(size_t(bands),[&](size_t band) {
        const int by1=int(band)*Band,by2=std::min(viewheight-1,by1+Band-1);
        for(const Candidate &k:candidates) {
            if(k.y2<by1 || k.y1>by2) continue;
            const PlaneDecal &d=*k.d;
            const bool print=d.shape&PrintBit,left=d.shape&LeftBit;
            const uint8_t *mask=print?printMask(d.shape&0x1f):(d.shape&PoolBit)?poolMask(d.shape&0x7f):splatMask(d.shape);
            double inverse=.5/k.radius,major=inverse/d.stretch;
            const uint8_t *colors=shadeTable(d.fluid,k.age,d.tone);
            fixed_t height=planes[size_t(k.first)]->height;
            for(size_t p=size_t(k.first);p<planes.size() && planes[p]->height==height;++p) {
                visplane_t *pl=planes[p];
                int px1=std::max(k.x1,pl->minx),px2=std::min(k.x2,pl->maxx);
                if(px1>px2) continue;
                RowLight *lit=&rowPool[p*size_t(viewheight)];
                int level=std::clamp((pl->lightlevel>>LIGHTSEGSHIFT)+extralight,0,LIGHTLEVELS-1);
                lighttable_t **table=zlight[level];
                fixed_t planeheight=std::abs(pl->height-viewz);
                double eye=std::fabs((pl->height-viewz)*Unit);
                for(int x=px1;x<=px2;++x) {
                    unsigned top=pl->top[x+1];
                    if(top==0xffff) continue;
                    // Rows where this column's floor ray is inside the stain circle.
                    double qa=rayX[x]*rayX[x]+rayY[x]*rayY[x],qb=-2*(k.rx*rayX[x]+k.ry*rayY[x]),qc=k.rx*k.rx+k.ry*k.ry-k.reach*k.reach*1.0201;
                    double disc=qb*qb-4*qa*qc;
                    if(disc<0) continue;
                    double root=std::sqrt(disc),far=(-qb+root)/(2*qa),nearest=std::max((-qb-root)/(2*qa),.5);
                    if(far<=nearest) continue;
                    double rowA=eye*focal/far,rowB=eye*focal/nearest;
                    int ya,yb;
                    if(d.ceiling) { ya=int(std::floor(centery-.5-rowB)); yb=int(std::ceil(centery-.5-rowA)); }
                    else { ya=int(std::floor(centery-.5+rowA)); yb=int(std::ceil(centery-.5+rowB)); }
                    ya=std::max({ya,k.y1,int(top),by1}); yb=std::min({yb,k.y2,int(pl->bottom[x+1]),by2});
                    for(int y=ya;y<=yb;++y) {
                        RowLight &row=lit[y];
                        if(row.frame!=rowFrame) {
                            fixed_t distance=FixedMul(planeheight,yslope[y]);
                            lighttable_t *map,*next; unsigned weight=R_PlaneTables(table,distance,&map,&next);
                            row={map,next,weight,rowFrame,distance*Unit};
                        }
                        double dist=row.dist;
                        double wx=vx+dist*rayX[x]-d.x,wy=vy+dist*rayY[x]-d.y;
                        double lx=(wx*d.c+wy*d.s)*major+.5,ly=(wy*d.c-wx*d.s)*inverse+.5;
                        if(lx<0 || ly<0 || lx>=1 || ly>=1) continue;
                        if(left) ly=std::nextafter(1.0,0.0)-ly;
                        int smooth=sampleMask(mask,lx,ly)+dither(x,y);
                        if(smooth<=8) continue;
                        uint8_t density=uint8_t(std::min(smooth,255));
                        if(print) { if(density<=k.threshold) continue; density=uint8_t(std::max(1,k.film*density/255)); }
                        uint8_t color=colors[density>>4];
                        if(k.wet && d.fluid==Red && density>=120 && !mirror.empty()) {
                            // Mirror row about the floor's far edge in this column.
                            int mirrored=2*int(top)-y-1;
                            if(mirrored>=0 && mirrored<int(top)) {
                                double cosine=eye/std::sqrt(eye*eye+dist*dist),g=1-cosine,f=.04+.96*g*g*g*g*g;
                                if(density<170) f*=.5;
                                // Fade with age as the pool dries (45 s of wetness).
                                f*=std::clamp(1-k.age/(35.0*45),0.0,1.0);
                                int tier=f<.06?-1:f<.12?0:f<.21?1:f<.32?2:f<.44?3:4;
                                if(tier>=0) {
                                    byte *dest=ylookup[y]+columnofs[x];
                                    uint8_t reflected=*(ylookup[mirrored]+columnofs[x]);
                                    *dest=mirror[size_t(tier)*65536+size_t(row.map[color])*256+reflected];
                                    if(light) light[dest-base]=0;
                                    continue;
                                }
                            }
                        }
                        if(k.wet && density>=170 && eye<dist*.3) color=sheen(color);
                        writePixel(x,y,color,row.map,row.next,row.weight,light,base);
                    }
                }
            }
        }
    });
}

namespace {

// Wall film, two octaves of smooth value noise bilinear between lattice
// points: a broad field (about 1/5 unit) shapes the wet patches, and a fine
// one (about 1/25 unit) adds a faint, soft-edged grain. No hard cutoff and no
// per-pixel dither, so nothing reads as a pixel grid. Down one screen column
// the across coordinate is fixed, so lattice rows are hashed only when the
// column crosses into a new one.
struct ColumnNoise {
    uint32_t seed=0,salt=0; double scale=0,ta=0; int a=0,b=0; bool ready=false; double top=0,bottom=0;
    ColumnNoise()=default;
    ColumnNoise(uint32_t index,uint32_t salt,double scale,double along):seed(index*2654435761u),salt(salt),scale(scale) {
        double fa=along*scale,ia=std::floor(fa); ta=fa-ia; ta=ta*ta*(3-2*ta); a=int(ia);
    }
    double lattice(int x,int y) const { return (hash(seed^uint32_t(x)*73856093u^uint32_t(y)*19349663u^salt)&255)/255.0; }
    double row(int y) const { return lattice(a,y)*(1-ta)+lattice(a+1,y)*ta; }
    double at(double height) {
        double fb=height*scale,ib=std::floor(fb),tb=fb-ib; tb=tb*tb*(3-2*tb);
        int next=int(ib);
        if(!ready || next!=b) {
            // Columns descend, so the new lattice row's lower neighbour is usually the old upper one.
            if(ready && next==b-1) { bottom=top; top=row(next); }
            else { top=row(next); bottom=row(next+1); }
            b=next; ready=true;
        }
        return top*(1-tb)+bottom*tb;
    }
};
struct ColumnGrain {
    ColumnNoise broad,fine;
    ColumnGrain()=default;
    ColumnGrain(uint32_t index,double along):broad(index,0,5,along),fine(index,0x9e37u,25,along) {}
    double at(double height) { return .75*broad.at(height)+.25*fine.at(height); }
};
// The horizontal half of sampleMask, fixed for a column.
struct MaskColumn {
    int ix,jx; double ax;
    explicit MaskColumn(double lx) {
        double fx=std::clamp(lx*MaskSize-.5,0.0,MaskSize-1.001);
        ix=int(fx); jx=std::min(ix+1,MaskSize-1); ax=fx-ix;
    }
    int sample(const uint8_t *mask,double ly) const {
        double fy=std::clamp(ly*MaskSize-.5,0.0,MaskSize-1.001);
        int iy=int(fy),jy=std::min(iy+1,MaskSize-1); double ay=fy-iy;
        double top=mask[iy*MaskSize+ix]*(1-ax)+mask[iy*MaskSize+jx]*ax,bottom=mask[jy*MaskSize+ix]*(1-ax)+mask[jy*MaskSize+jx]*ax;
        return int(top*(1-ay)+bottom*ay+.5);
    }
};
// Where a splat can show at all: per texel column, runs of sample rows whose
// bilinear footprint (this column or the next, this row or the next) holds a
// texel thick enough for film; a thinner sample never passes the film
// threshold. Columns skip straight past everything outside these runs.
constexpr int FilmTexel=12;
struct MaskRuns { std::vector<uint8_t> rows; uint16_t offset[MaskSize+1]; };
MaskRuns splatRuns[SplatShapes];
bool splatRunsReady=false;
void buildSplatRuns() {
    splatRunsReady=true;
    for(int shape=0;shape<SplatShapes;++shape) {
        const uint8_t *mask=splatMask(shape); MaskRuns &runs=splatRuns[shape];
        for(int ix=0;ix<MaskSize;++ix) {
            runs.offset[ix]=uint16_t(runs.rows.size());
            int jx=std::min(ix+1,MaskSize-1);
            auto thick=[&](int iy) { iy=std::min(iy,MaskSize-1); return mask[iy*MaskSize+ix]>=FilmTexel || mask[iy*MaskSize+jx]>=FilmTexel; };
            for(int iy=0;iy<MaskSize;) {
                if(!thick(iy) && !thick(iy+1)) { ++iy; continue; }
                int first=iy;
                while(iy<MaskSize && (thick(iy) || thick(iy+1))) ++iy;
                runs.rows.push_back(uint8_t(first)); runs.rows.push_back(uint8_t(iy-1));
            }
        }
        runs.offset[MaskSize]=uint16_t(runs.rows.size());
    }
}
}

const void *Native_GoreWallSide(int line,int side) {
    if(!enabled() || gamestate!=GS_LEVEL || !frameReady) return nullptr;
    State &g=state(); size_t slot=size_t(line)*2+size_t(side);
    if(line<0 || slot>=g.sideHead.size() || g.sideHead[slot]<0) return nullptr;
    if(preparedSides.size()!=g.sideHead.size()) preparedSides.assign(g.sideHead.size(),PreparedSide{});
    if(!splatRunsReady) buildSplatRuns();
    PreparedSide &prepared=preparedSides[slot];
    if(prepared.view==preparedView) return prepared.count?&prepared:nullptr;
    // Called serially while walls are stored; columns read the result later,
    // possibly in parallel. The pool is reserved for every stain, so entries
    // never move during a view.
    prepared={preparedView,int(preparedWalls.size()),0};
    for(int32_t i=g.sideHead[slot];i>=0;i=g.walls[i].next) {
        const WallDecal &d=g.walls[i];
        double t=std::max(0.0,renderTime-d.born),halfWidth=d.radius*d.stretch;
        PreparedWall w{splatMask(d.shape),shadeTable(d.fluid,int(t),d.tone),d.u,halfWidth,d.radius,anchorHeight(d)+d.z,0,-halfWidth,halfWidth,uint32_t(i),0,d.shape%SplatShapes,{}};
        w.start=w.center-d.radius*.2;
        // Drips: up to four thin rivulets per stain, each tapering from the
        // stain down to a heavier bead at its tip.
        for(int k=0,n=rivulets(d);k<n;++k) {
            Rivulet r=rivulet(d,i,k);
            double run=rivuletRun(r,t);
            if(run<.5) continue;
            w.run[w.runs++]={r.offset,r.width,run};
            w.uLo=std::min(w.uLo,r.offset-r.width*1.3); w.uHi=std::max(w.uHi,r.offset+r.width*1.3);
        }
        // Columns compare against these bounds before the exact tests; the
        // margin keeps rounding from ever dropping a column that is drawn.
        w.uLo=w.u+w.uLo-1e-6; w.uHi=w.u+w.uHi+1e-6;
        preparedWalls.push_back(w);
        ++prepared.count;
    }
    std::reverse(preparedWalls.begin()+prepared.begin,preparedWalls.end());
    return prepared.count?&prepared:nullptr;
}

// Thread-safe: everything per column arrives as arguments, so columns can be
// drawn in parallel after their walls. Stains are visited in winning order and
// each pixel takes the first that covers it, the same result as drawing them
// all over each other.
void Native_GoreWallColumn(const void *handle,int32_t ufixed,int yl,int yh,int x,int32_t iscale,
                           const void *colormap,const void *colormapNext,unsigned lightweight) {
    if(yl>yh || !handle) return;
    const auto &side=*static_cast<const PreparedSide *>(handle);
    uint16_t *light=R_NativeLightPixels(); const byte *base=Native_RenderPixels();
    const auto *map=static_cast<const lighttable_t *>(colormap);
    const auto *next=light?static_cast<const lighttable_t *>(colormapNext):nullptr; unsigned weight=light?lightweight:0;
    double u=ufixed*Unit,step=iscale*Unit,eye=viewz*Unit;
    if(step<=0) return;
    yl=std::max(yl,0); yh=std::min(yh,NATIVE_MAX_HEIGHT-1);
    if(yl>yh) return;
    uint64_t covered[NATIVE_MAX_HEIGHT/64]={};
    int open=yh-yl+1;
    auto take=[&](int y) { covered[(y-yl)>>6]|=uint64_t(1)<<((y-yl)&63); --open; };
    // First row at or after y that no stain has covered yet; whole covered
    // words are skipped at once.
    auto nextOpen=[&](int y) {
        for(int r=y-yl;r<=yh-yl;r=(r|63)+1)
            if(uint64_t word=~covered[r>>6]>>(r&63)) return yl+r+__builtin_ctzll(word);
        return yh+1;
    };
    const PreparedWall *walls=preparedWalls.data()+side.begin;
    for(int n=0;n<side.count && open;++n) {
        const PreparedWall &d=walls[n];
        if(u<d.uLo || u>d.uHi) continue;
        double du=u-d.u;
        const PreparedRun *runs[4]; int count=0; double lowest=d.center-d.radius;
        for(int k=0;k<d.runs;++k) {
            if(std::fabs(du-d.run[k].offset)>d.run[k].width*1.3) continue;
            runs[count++]=&d.run[k];
            lowest=std::min(lowest,d.center-d.radius*.2-d.run[k].length-.6);
        }
        bool inside=std::fabs(du)<=d.halfWidth;
        if(!inside && !count) continue;
        double top=d.center+d.radius;
        int ya=std::max(yl,int(std::floor(centery+(eye-top)/step))),yb=std::min(yh,int(std::ceil(centery+(eye-lowest)/step)));
        if(ya>yb) continue;
        // Screen row of a height in the stain (ly 0 top, 1 bottom) and of a
        // height on the wall, kept within reach of this stain's rows.
        auto rowOf=[&](double height) { return std::clamp(centery+(eye-height)/step,ya-2.0,yb+2.0); };
        if(inside) {
            // Film first: rows where its density is above zero are this
            // stain's; the drips below only fill rows the film left open.
            MaskColumn column((u-d.u)/d.halfWidth*.5+.5);
            ColumnGrain film(d.index,du);
            const MaskRuns &shape=splatRuns[d.shape];
            for(int r=shape.offset[column.ix];r<shape.offset[column.ix+1];r+=2) {
                int first=shape.rows[r],last=shape.rows[r+1];
                // Sample row iy covers ly in [(iy+.5)/MaskSize,(iy+1.5)/MaskSize);
                // a margin row each side absorbs rounding.
                int lo=first==0?ya:std::max(ya,int(std::floor(rowOf(d.center-(2*(first+.5)/MaskSize-1)*d.radius)))-1);
                int hi=last>=MaskSize-2?yb:std::min(yb,int(std::ceil(rowOf(d.center-(2*(last+1.5)/MaskSize-1)*d.radius)))+1);
                for(int y=nextOpen(lo);y<=hi;y=nextOpen(y+1)) {
                    double z=eye-(y-centery)*step,ly=(d.center-z)/d.radius*.5+.5;
                    if(ly<0 || ly>=1) continue;
                    // Wall film: no per-pixel dither; the grain alone breaks
                    // the edge. The grain only thins the film (factor at most
                    // 1), so a mask too faint to show skips it.
                    int coverage=column.sample(d.mask,ly);
                    if(int(coverage*.8)<=8) continue;
                    int density=int(coverage*(.55+.45*film.at(z))*.8);
                    if(density>8) { writePixel(x,y,d.tones[std::min(density,255)>>4],map,next,weight,light,base); take(y); }
                }
            }
        }
        if(!count || !open) continue;
        // Each rivulet's grain runs along its own centre line.
        ColumnGrain drips[4]; double longest=0;
        for(int k=0;k<count;++k) { drips[k]=ColumnGrain(d.index,du-runs[k]->offset); longest=std::max(longest,runs[k]->length); }
        int lo=std::max(ya,int(std::floor(rowOf(d.start)))-1),hi=std::min(yb,int(std::ceil(rowOf(d.start-longest)))+1);
        for(int y=nextOpen(lo);y<=hi;y=nextOpen(y+1)) {
            double z=eye-(y-centery)*step;
            int density=0;
            for(int k=0;k<count && !density;++k) {
                double along=d.start-z;
                if(along<0 || along>runs[k]->length) continue;
                bool bead=along>runs[k]->length-1.2;
                double width=runs[k]->width*(bead?1.25:1-.45*along/std::max(runs[k]->length,1.0));
                if(std::fabs(du-runs[k]->offset)<width) density=bead?225:(drips[k].at(z)>=.9?0:165);
            }
            if(density) { writePixel(x,y,d.tones[std::min(density,255)>>4],map,next,weight,light,base); take(y); }
        }
    }
}

