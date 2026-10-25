// SPDX-License-Identifier: GPL-2.0-only
#include "doomstat.h"
#include "i_system.h"
#include "p_local.h"
#include "p_saveg.h"
#include "m_random.h"
#include "m_fixed.h"
#include "z_zone.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "g_game.h"
#include "Native.h"
#include <inttypes.h>

extern int gametic, prndindex;
extern void G_BuildTiccmd(ticcmd_t *);

static void keyboard_command_test(void)
{
    extern boolean gamekeydown[256];
    extern int key_up,key_down,key_left,key_right,key_strafeleft,key_straferight,key_use,key_fire,key_strafe,key_speed;
    int saved[]={key_up,key_down,key_left,key_right,key_strafeleft,key_straferight,key_use,key_fire,key_strafe,key_speed};
    key_up=KEY_UPARROW;key_down=KEY_DOWNARROW;key_left=KEY_LEFTARROW;key_right=KEY_RIGHTARROW;
    key_strafeleft=',';key_straferight='.';key_use=' ';key_fire=KEY_RCTRL;key_strafe=KEY_RALT;key_speed=KEY_RSHIFT;
    const int keys[]={'w','s','a','d',' ',KEY_ENTER};
    for(int n=0;n<6;++n) {
        memset(gamekeydown,0,256*sizeof(*gamekeydown));gamekeydown[keys[n]]=true;ticcmd_t cmd;G_BuildTiccmd(&cmd);
        if(n<2)assert(cmd.forwardmove==(n==0?25:-25) && cmd.sidemove==0 && cmd.angleturn==0);
        else if(n<4)assert(cmd.sidemove==(n==2?-24:24) && cmd.forwardmove==0 && cmd.angleturn==0);
        else assert((cmd.buttons&BT_USE) && !(cmd.buttons&BT_ATTACK));
    }
    memset(gamekeydown,0,256*sizeof(*gamekeydown));gamekeydown['w']=gamekeydown[KEY_UPARROW]=true;
    ticcmd_t cmd;G_BuildTiccmd(&cmd);assert(cmd.forwardmove==25); // aliases never double movement
    memset(gamekeydown,0,256*sizeof(*gamekeydown));G_BuildTiccmd(&cmd);assert(!cmd.forwardmove && !cmd.sidemove && !cmd.angleturn && !(cmd.buttons&BT_USE));
    key_up=saved[0];key_down=saved[1];key_left=saved[2];key_right=saved[3];key_strafeleft=saved[4];key_straferight=saved[5];key_use=saved[6];key_fire=saved[7];key_strafe=saved[8];key_speed=saved[9];
    printf("Keyboard: WASD native commands, Space/Enter Use, alias overlap and key release passed\n");
}
int Native_CoreTest(void)
{
    keyboard_command_test();
    M_ClearRandom();
    const int random_reference[]={8,109,220,222,241,149,107,75};
    for(unsigned i=0;i<sizeof(random_reference)/sizeof(*random_reference);++i)
        assert(P_Random()==random_reference[i]);
    M_ClearRandom(); assert(M_Random()==8); assert(P_Random()==8);
    assert(FixedMul(-FRACUNIT,FRACUNIT)==-FRACUNIT);
    assert(FixedDiv(-FRACUNIT,FRACUNIT)==-FRACUNIT);
    assert(FixedDiv(INT32_MIN,0)==INT32_MIN);
    assert(FixedDiv(INT32_MAX,0)==INT32_MAX);
    Z_Init();
    void *owner=NULL;
    for(int n=1;n<129;++n) { void *p=Z_Malloc(n,PU_STATIC,&owner); assert(((uintptr_t)p&7)==0); assert(owner==p); Z_Free(p); assert(owner==NULL); }
    Z_CheckHeap();
    byte buffer[8192]; memset(buffer,0,sizeof(buffer));
    memset(players,0,sizeof(players)); memset(playeringame,0,sizeof(playeringame));
    playeringame[0]=true; players[0].health=37; players[0].armorpoints=99;
    players[0].cmd.forwardmove=-50; players[0].cmd.sidemove=40; players[0].cmd.angleturn=-1280;
    players[0].psprites[0].state=&states[S_PISTOL]; players[0].psprites[0].tics=7;
    players[0].ammo[am_clip]=23; players[0].weaponowned[wp_shotgun]=true;
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+sizeof(buffer),true); P_ArchivePlayers();
    assert(save_p-buffer==280);
    memset(players,0,sizeof(players)); save_p=buffer; P_SetSaveBuffer(buffer,buffer+280,false); P_UnArchivePlayers();
    assert(players[0].health==37 && players[0].armorpoints==99);
    assert(players[0].cmd.forwardmove==-50 && players[0].cmd.angleturn==-1280);
    assert(players[0].psprites[0].state==&states[S_PISTOL] && players[0].psprites[0].tics==7);
    assert(players[0].ammo[am_clip]==23 && players[0].weaponowned[wp_shotgun]);
    assert(!players[0].mo && !players[0].message && !players[0].attacker);
    sectors=calloc(1,sizeof(*sectors)); numsectors=1; sectors[0].ceilingheight=128*FRACUNIT;
    subsectors=calloc(1,sizeof(*subsectors)); numsubsectors=1; subsectors[0].sector=sectors; numnodes=0;
    P_InitThinkers();
    mobj_t *mo=Z_Malloc(sizeof(*mo),PU_LEVEL,NULL); memset(mo,0,sizeof(*mo));
    mo->type=MT_PLAYER; mo->state=&states[S_PLAY]; mo->player=&players[0]; mo->health=37;
    mo->x=-100*FRACUNIT; mo->y=50*FRACUNIT; mo->momx=-FRACUNIT; mo->flags=MF_NOSECTOR|MF_NOBLOCKMAP;
    mo->thinker.function.acp1=(actionf_p1)P_MobjThinker; P_AddThinker(&mo->thinker);
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+sizeof(buffer),true); P_ArchiveThinkers();
    size_t length=save_p-buffer; assert(length==161);
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+length,false); P_UnArchiveThinkers();
    mo=(mobj_t *)thinkercap.next;
    assert(mo->player==&players[0] && players[0].mo==mo && mo->health==37);
    assert(mo->x==-100*FRACUNIT && mo->momx==-FRACUNIT && mo->state==&states[S_PLAY]);
    assert(!mo->target && !mo->tracer && mo->info==&mobjinfo[MT_PLAYER]);
    assert(mo->subsector==subsectors && mo->ceilingz==128*FRACUNIT);
    // World values use signed 16-bit map units on disk, including negative offsets.
    Z_FreeTags(PU_LEVEL,PU_PURGELEVEL-1);
    free(sectors); sectors=calloc(9,sizeof(*sectors)); numsectors=9;
    lines=calloc(1,sizeof(*lines)); numlines=1;
    sides=calloc(2,sizeof(*sides)); numsides=2;
    sectors[0].floorheight=-64*FRACUNIT; sectors[0].ceilingheight=192*FRACUNIT;
    sectors[0].floorpic=3; sectors[0].ceilingpic=4; sectors[0].lightlevel=160; sectors[0].special=8; sectors[0].tag=17;
    lines[0].sidenum[0]=0; lines[0].sidenum[1]=1; lines[0].flags=4; lines[0].special=62; lines[0].tag=17;
    sides[0].textureoffset=-32*FRACUNIT; sides[1].rowoffset=-8*FRACUNIT; sides[1].midtexture=9;
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+sizeof(buffer),true); P_ArchiveWorld();
    length=save_p-buffer; assert(length==152);
    memset(sectors,0,9*sizeof(*sectors)); memset(sides,0,2*sizeof(*sides)); lines[0].flags=0;
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+length,false); P_UnArchiveWorld();
    assert(sectors[0].floorheight==-64*FRACUNIT && sectors[0].ceilingheight==192*FRACUNIT);
    assert(sectors[0].special==8 && sectors[0].tag==17 && lines[0].flags==4 && lines[0].special==62);
    assert(sides[0].textureoffset==-32*FRACUNIT && sides[1].rowoffset==-8*FRACUNIT && sides[1].midtexture==9);

    P_InitThinkers(); memset(activeceilings,0,sizeof(activeceilings)); memset(activeplats,0,sizeof(activeplats));
    #define SPECIAL(type,name,fn,index) type *name=Z_Malloc(sizeof(type),PU_LEVEL,NULL); memset(name,0,sizeof(*name)); name->sector=&sectors[index]; name->thinker.function.acp1=(actionf_p1)fn; P_AddThinker(&name->thinker)
    SPECIAL(ceiling_t,ceiling,T_MoveCeiling,1); ceiling->topheight=200*FRACUNIT; ceiling->direction=-1; ceiling->crush=true; P_AddActiveCeiling(ceiling);
    SPECIAL(vldoor_t,door,T_VerticalDoor,2); door->topcountdown=149; door->topheight=128*FRACUNIT;
    SPECIAL(floormove_t,floor,T_MoveFloor,3); floor->texture=17; floor->newspecial=5; floor->floordestheight=-16*FRACUNIT;
    SPECIAL(plat_t,plat,T_PlatRaise,4); plat->low=-8*FRACUNIT; plat->high=64*FRACUNIT; plat->count=70; P_AddActivePlat(plat);
    SPECIAL(lightflash_t,flash,T_LightFlash,5); flash->count=9; flash->minlight=64; flash->maxtime=32;
    SPECIAL(strobe_t,strobe,T_StrobeFlash,6); strobe->brighttime=5; strobe->darktime=35; strobe->maxlight=192;
    SPECIAL(glow_t,glow,T_Glow,7); glow->minlight=32; glow->maxlight=160; glow->direction=-1;
    SPECIAL(ceiling_t,stasis,NULL,8); stasis->olddirection=-1; stasis->tag=19; P_AddActiveCeiling(stasis);
    #undef SPECIAL
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+sizeof(buffer),true); P_ArchiveSpecials();
    length=save_p-buffer; assert(length==369);
    Z_FreeTags(PU_LEVEL,PU_PURGELEVEL-1); P_InitThinkers();
    memset(activeceilings,0,sizeof(activeceilings)); memset(activeplats,0,sizeof(activeplats));
    save_p=buffer; P_SetSaveBuffer(buffer,buffer+length,false); P_UnArchiveSpecials(); assert(save_p==buffer+length);
    thinker_t *th=thinkercap.next;
    #define RESTORED(type,name,fn,index) name=(type *)th; assert(th!=&thinkercap && th->function.acp1==(actionf_p1)fn && name->sector==&sectors[index]); th=th->next
    RESTORED(ceiling_t,ceiling,T_MoveCeiling,1); assert(ceiling->topheight==200*FRACUNIT && ceiling->direction==-1 && ceiling->crush && activeceilings[0]==ceiling);
    RESTORED(vldoor_t,door,T_VerticalDoor,2); assert(door->topcountdown==149 && sectors[2].specialdata==door);
    RESTORED(floormove_t,floor,T_MoveFloor,3); assert(floor->texture==17 && floor->newspecial==5 && floor->floordestheight==-16*FRACUNIT);
    RESTORED(plat_t,plat,T_PlatRaise,4); assert(plat->low==-8*FRACUNIT && plat->high==64*FRACUNIT && plat->count==70 && activeplats[0]==plat);
    RESTORED(lightflash_t,flash,T_LightFlash,5); assert(flash->count==9 && flash->minlight==64 && flash->maxtime==32);
    RESTORED(strobe_t,strobe,T_StrobeFlash,6); assert(strobe->brighttime==5 && strobe->darktime==35 && strobe->maxlight==192);
    RESTORED(glow_t,glow,T_Glow,7); assert(glow->minlight==32 && glow->maxlight==160 && glow->direction==-1);
    RESTORED(ceiling_t,stasis,NULL,8); assert(stasis->olddirection==-1 && stasis->tag==19 && activeceilings[1]==stasis);
    #undef RESTORED
    assert(th==&thinkercap);
    printf("Core: original RNG, fixed-point boundaries, allocator alignment, ILP32 players/actors/world/all seven specials and ceiling stasis round trips passed\n");
    return 0;
}
