// SPDX-License-Identifier: GPL-2.0-only
// Original indexed psprites in their own 320x200 artwork coordinate system.
#include "Native.h"
extern "C" {
#include "doomstat.h"
#include "r_local.h"
#include "w_wad.h"
#include "m_swap.h"
#include "z_zone.h"
#include "d_items.h"
}
#include <array>
#include <algorithm>
#include <cmath>
namespace {
std::array<uint16_t,320*200> pixels;
int bottomOffset=0;
}
void Native_ViewmodelBegin(void *player) {
    pixels.fill(0xffff); bottomOffset=0;
    const auto *p=static_cast<player_t *>(player);
    int sprite=states[weaponinfo[p->readyweapon].readystate].sprite;
    const spritedef_t &definition=sprites[sprite];
    int bottom=200;
    // Some original art includes 32 extra rows formerly cropped by the screen.
    // Normalize the entire weapon animation together; never recenter each frame.
    for(int f=0;f<definition.numframes;++f) {
        const auto *patch=static_cast<patch_t *>(W_CacheLumpNum(firstspritelump+definition.spriteframes[f].lump[0],PU_CACHE));
        bottom=std::max(bottom,32-SHORT(patch->topoffset)+SHORT(patch->height));
    }
    bottomOffset=bottom-200;
}
void Native_ViewmodelPatch(int lump,int flip,int32_t sx,int32_t sy,const uint8_t *map) {
    const auto *patch=static_cast<patch_t *>(W_CacheLumpNum(firstspritelump+lump,PU_CACHE));
    int width=SHORT(patch->width),left=int(std::llround(double(sx)/FRACUNIT))-SHORT(patch->leftoffset);
    int top=int(std::llround(double(sy)/FRACUNIT))-SHORT(patch->topoffset)-bottomOffset;
    for(int x=0;x<width;++x) {
        int dest=left+(flip?width-1-x:x); if(dest<0 || dest>=320) continue;
        const auto *post=reinterpret_cast<const uint8_t *>(patch)+LONG(patch->columnofs[x]);
        while(post[0]!=255) {
            for(int y=0;y<post[1];++y) {
                int row=top+post[0]+y;
                if(row>=0 && row<200) pixels[size_t(row)*320+dest]=map?uint16_t(map[post[3+y]]):0x100; // 0x100: fuzz
            }
            post+=post[1]+4;
        }
    }
}
const uint16_t *Native_ViewmodelPixels(void) { return pixels.data(); }
const uint8_t *Native_ViewmodelFuzzMap(void) { return colormaps?colormaps+6*256:nullptr; }
