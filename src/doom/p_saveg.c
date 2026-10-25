// Native macOS port: modified 2026-09-30 for arm64/LP64 correctness; GPL-2.0.
// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Archiving: SaveGame I/O.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: p_tick.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";

#include "i_system.h"
#include "z_zone.h"
#include "p_local.h"

// State.
#include "doomstat.h"
#include "r_state.h"

byte*		save_p;


static byte *save_begin, *save_end;
static boolean save_writing;
void P_SetSaveBuffer(byte *begin, byte *end, boolean writing)
{
    save_begin = begin; save_end = end; save_writing = writing;
}
void P_SaveCheck(size_t bytes)
{
    if (!save_begin || save_p < save_begin || save_p > save_end || bytes > (size_t)(save_end-save_p))
        I_Error("Truncated or oversized savegame");
}
static void PadSave(void)
{
    size_t pad = (4 - ((size_t)(save_p-save_begin)&3))&3;
    P_SaveCheck(pad);
    if (save_writing) memset(save_p,0,pad);
    save_p += pad;
}
#define PADSAVEP() PadSave()
static byte ReadSaveByte(void) { P_SaveCheck(1); return *save_p++; }
static void WriteSaveByte(byte v) { P_SaveCheck(1); *save_p++=v; }
static intptr_t SaveIndex(const void *p, int limit)
{
    intptr_t i=(intptr_t)p;
    if(i<0 || i>=limit) I_Error("Invalid savegame object index");
    return i;
}
#include "p_savecodec.h"

//
// P_ArchivePlayers
//
void P_ArchivePlayers (void)
{
    for(int i=0; i<MAXPLAYERS; ++i) {
        if(!playeringame[i]) continue;
        PADSAVEP();
        player_t copy=players[i];
        for(int j=0;j<NUMPSPRITES;++j) if(copy.psprites[j].state)
            copy.psprites[j].state=(state_t *)(uintptr_t)(copy.psprites[j].state-states);
        SaveWrite_player_t(&copy);
    }
}

//
// P_UnArchivePlayers
//
void P_UnArchivePlayers (void)
{
    int		i;
    int		j;
	
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
	if (!playeringame[i])
	    continue;
	
	PADSAVEP();

	SaveRead_player_t(&players[i]);
	
	// will be set when unarc thinker
	players[i].mo = NULL;	
	players[i].message = NULL;
	players[i].attacker = NULL;

	for (j=0 ; j<NUMPSPRITES ; j++)
	{
	    if (players[i]. psprites[j].state)
	    {
		players[i]. psprites[j].state 
		    = &states[SaveIndex(players[i].psprites[j].state, NUMSTATES)];
	    }
	}
    }
}


//
// P_ArchiveWorld
//
void P_ArchiveWorld (void)
{
    int			i;
    int			j;
    sector_t*		sec;
    line_t*		li;
    side_t*		si;
    short*		put;
	
    size_t bytes=(size_t)numsectors*14;
    for(int k=0;k<numlines;++k) { bytes+=6; for(int j=0;j<2;++j) if(lines[k].sidenum[j]!=-1) bytes+=10; }
    P_SaveCheck(bytes);
    put = (short *)save_p;
    
    // do sectors
    for (i=0, sec = sectors ; i<numsectors ; i++,sec++)
    {
	*put++ = sec->floorheight >> FRACBITS;
	*put++ = sec->ceilingheight >> FRACBITS;
	*put++ = sec->floorpic;
	*put++ = sec->ceilingpic;
	*put++ = sec->lightlevel;
	*put++ = sec->special;		// needed?
	*put++ = sec->tag;		// needed?
    }

    
    // do lines
    for (i=0, li = lines ; i<numlines ; i++,li++)
    {
	*put++ = li->flags;
	*put++ = li->special;
	*put++ = li->tag;
	for (j=0 ; j<2 ; j++)
	{
	    if (li->sidenum[j] == -1)
		continue;
	    
	    si = &sides[li->sidenum[j]];

	    *put++ = si->textureoffset >> FRACBITS;
	    *put++ = si->rowoffset >> FRACBITS;
	    *put++ = si->toptexture;
	    *put++ = si->bottomtexture;
	    *put++ = si->midtexture;	
	}
    }
	
    save_p = (byte *)put;
}



//
// P_UnArchiveWorld
//
void P_UnArchiveWorld (void)
{
    int			i;
    int			j;
    sector_t*		sec;
    line_t*		li;
    side_t*		si;
    short*		get;
	
    size_t bytes=(size_t)numsectors*14;
    for(int k=0;k<numlines;++k) { bytes+=6; for(int j=0;j<2;++j) if(lines[k].sidenum[j]!=-1) bytes+=10; }
    P_SaveCheck(bytes);
    get = (short *)save_p;
    
    // do sectors
    for (i=0, sec = sectors ; i<numsectors ; i++,sec++)
    {
	sec->floorheight = (int32_t)*get++ * FRACUNIT;
	sec->ceilingheight = (int32_t)*get++ * FRACUNIT;
	sec->floorpic = *get++;
	sec->ceilingpic = *get++;
	sec->lightlevel = *get++;
	sec->special = *get++;		// needed?
	sec->tag = *get++;		// needed?
	sec->specialdata = 0;
	sec->soundtarget = 0;
    }
    
    // do lines
    for (i=0, li = lines ; i<numlines ; i++,li++)
    {
	li->flags = *get++;
	li->special = *get++;
	li->tag = *get++;
	for (j=0 ; j<2 ; j++)
	{
	    if (li->sidenum[j] == -1)
		continue;
	    si = &sides[li->sidenum[j]];
	    si->textureoffset = (int32_t)*get++ * FRACUNIT;
	    si->rowoffset = (int32_t)*get++ * FRACUNIT;
	    si->toptexture = *get++;
	    si->bottomtexture = *get++;
	    si->midtexture = *get++;
	}
    }
    save_p = (byte *)get;	
}





//
// Thinkers
//
typedef enum
{
    tc_end,
    tc_mobj

} thinkerclass_t;



//
// P_ArchiveThinkers
//
void P_ArchiveThinkers (void)
{
    thinker_t*		th;
    mobj_t*		mobj;
    mobj_t local;
	
    // save off the current thinkers
    for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    {
	if (th->function.acp1 == (actionf_p1)P_MobjThinker)
	{
	    WriteSaveByte(tc_mobj);
	    PADSAVEP();
	    mobj = &local;
	    memcpy (mobj, th, sizeof(*mobj));
	
	    mobj->state = (state_t *)(intptr_t)(mobj->state - states);
	    
	    if (mobj->player)
		mobj->player = (player_t *)(uintptr_t)((mobj->player-players) + 1);
	    SaveWrite_mobj_t(mobj);
	    continue;
	}
		
	// I_Error ("P_ArchiveThinkers: Unknown thinker function");
    }

    // add a terminating marker
    WriteSaveByte(tc_end);	
}



//
// P_UnArchiveThinkers
//
void P_UnArchiveThinkers (void)
{
    byte		tclass;
    thinker_t*		currentthinker;
    thinker_t*		next;
    mobj_t*		mobj;
    
    // remove all the current thinkers
    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap)
    {
	next = currentthinker->next;
	
	if (currentthinker->function.acp1 == (actionf_p1)P_MobjThinker)
	    P_RemoveMobj ((mobj_t *)currentthinker);
	else
	    Z_Free (currentthinker);

	currentthinker = next;
    }
    P_InitThinkers ();
	
    // read in saved thinkers
    while (1)
    {
	tclass = ReadSaveByte();
	switch (tclass)
	{
	  case tc_end:
	    return; 	// end of list
			
	  case tc_mobj:
	    PADSAVEP();
	    mobj = Z_Malloc (sizeof(*mobj), PU_LEVEL, NULL);
	    SaveRead_mobj_t(mobj);
	    mobj->state = &states[SaveIndex(mobj->state, NUMSTATES)];
	    mobj->target = NULL;
	    if (mobj->player)
	    {
		mobj->player = &players[SaveIndex((void *)((uintptr_t)mobj->player-1), MAXPLAYERS)];
		mobj->player->mo = mobj;
	    }
	    P_SetThingPosition (mobj);
	    if ((unsigned)mobj->type >= NUMMOBJTYPES) I_Error("Invalid savegame actor type");
	    mobj->info = &mobjinfo[mobj->type];
            mobj->tracer = NULL;
	    mobj->floorz = mobj->subsector->sector->floorheight;
	    mobj->ceilingz = mobj->subsector->sector->ceilingheight;
	    mobj->thinker.function.acp1 = (actionf_p1)P_MobjThinker;
	    P_AddThinker (&mobj->thinker);
	    break;
			
	  default:
	    I_Error ("Unknown tclass %i in savegame",tclass);
	}
	
    }

}


//
// P_ArchiveSpecials
//
enum
{
    tc_ceiling,
    tc_door,
    tc_floor,
    tc_plat,
    tc_flash,
    tc_strobe,
    tc_glow,
    tc_endspecials

} specials_e;	



//
// Things to handle:
//
// T_MoveCeiling, (ceiling_t: sector_t * swizzle), - active list
// T_VerticalDoor, (vldoor_t: sector_t * swizzle),
// T_MoveFloor, (floormove_t: sector_t * swizzle),
// T_LightFlash, (lightflash_t: sector_t * swizzle),
// T_StrobeFlash, (strobe_t: sector_t *),
// T_Glow, (glow_t: sector_t *),
// T_PlatRaise, (plat_t: sector_t *), - active list
//
void P_ArchiveSpecials (void)
{
    ceiling_t local_ceiling;
    vldoor_t local_door;
    floormove_t local_floor;
    plat_t local_plat;
    lightflash_t local_flash;
    strobe_t local_strobe;
    glow_t local_glow;
    thinker_t*		th;
    ceiling_t*		ceiling;
    vldoor_t*		door;
    floormove_t*	floor;
    plat_t*		plat;
    lightflash_t*	flash;
    strobe_t*		strobe;
    glow_t*		glow;
    int			i;
	
    // save off the current thinkers
    for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    {
	if (th->function.acv == (actionf_v)NULL)
	{
	    for (i = 0; i < MAXCEILINGS;i++)
		if (activeceilings[i] == (ceiling_t *)th)
		    break;
	    
	    if (i<MAXCEILINGS)
	    {
		WriteSaveByte(tc_ceiling);
		PADSAVEP();
		ceiling = &local_ceiling;
		memcpy (ceiling, th, sizeof(*ceiling));
		
		ceiling->sector = (sector_t *)(uintptr_t)(ceiling->sector - sectors);
        SaveWrite_ceiling_t(ceiling);
	    }
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_MoveCeiling)
	{
	    WriteSaveByte(tc_ceiling);
	    PADSAVEP();
	    ceiling = &local_ceiling;
	    memcpy (ceiling, th, sizeof(*ceiling));
	    
	    ceiling->sector = (sector_t *)(uintptr_t)(ceiling->sector - sectors);
        SaveWrite_ceiling_t(ceiling);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_VerticalDoor)
	{
	    WriteSaveByte(tc_door);
	    PADSAVEP();
	    door = &local_door;
	    memcpy (door, th, sizeof(*door));
	    
	    door->sector = (sector_t *)(uintptr_t)(door->sector - sectors);
        SaveWrite_vldoor_t(door);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_MoveFloor)
	{
	    WriteSaveByte(tc_floor);
	    PADSAVEP();
	    floor = &local_floor;
	    memcpy (floor, th, sizeof(*floor));
	    
	    floor->sector = (sector_t *)(uintptr_t)(floor->sector - sectors);
        SaveWrite_floormove_t(floor);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_PlatRaise)
	{
	    WriteSaveByte(tc_plat);
	    PADSAVEP();
	    plat = &local_plat;
	    memcpy (plat, th, sizeof(*plat));
	    
	    plat->sector = (sector_t *)(uintptr_t)(plat->sector - sectors);
        SaveWrite_plat_t(plat);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_LightFlash)
	{
	    WriteSaveByte(tc_flash);
	    PADSAVEP();
	    flash = &local_flash;
	    memcpy (flash, th, sizeof(*flash));
	    
	    flash->sector = (sector_t *)(uintptr_t)(flash->sector - sectors);
        SaveWrite_lightflash_t(flash);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_StrobeFlash)
	{
	    WriteSaveByte(tc_strobe);
	    PADSAVEP();
	    strobe = &local_strobe;
	    memcpy (strobe, th, sizeof(*strobe));
	    
	    strobe->sector = (sector_t *)(uintptr_t)(strobe->sector - sectors);
        SaveWrite_strobe_t(strobe);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_Glow)
	{
	    WriteSaveByte(tc_glow);
	    PADSAVEP();
	    glow = &local_glow;
	    memcpy (glow, th, sizeof(*glow));
	    
	    glow->sector = (sector_t *)(uintptr_t)(glow->sector - sectors);
        SaveWrite_glow_t(glow);
	    continue;
	}
    }
	
    // add a terminating marker
    WriteSaveByte(tc_endspecials);	

}


//
// P_UnArchiveSpecials
//
void P_UnArchiveSpecials (void)
{
    byte		tclass;
    ceiling_t*		ceiling;
    vldoor_t*		door;
    floormove_t*	floor;
    plat_t*		plat;
    lightflash_t*	flash;
    strobe_t*		strobe;
    glow_t*		glow;
	
	
    // read in saved thinkers
    while (1)
    {
	tclass = ReadSaveByte();
	switch (tclass)
	{
	  case tc_endspecials:
	    return;	// end of list
			
	  case tc_ceiling:
	    PADSAVEP();
	    ceiling = Z_Malloc (sizeof(*ceiling), PU_LEVEL, NULL);
	    SaveRead_ceiling_t(ceiling);
	    ceiling->sector = &sectors[SaveIndex(ceiling->sector, numsectors)];
	    ceiling->sector->specialdata = ceiling;

	    if (ceiling->thinker.function.acp1)
		ceiling->thinker.function.acp1 = (actionf_p1)T_MoveCeiling;

	    P_AddThinker (&ceiling->thinker);
	    P_AddActiveCeiling(ceiling);
	    break;
				
	  case tc_door:
	    PADSAVEP();
	    door = Z_Malloc (sizeof(*door), PU_LEVEL, NULL);
	    SaveRead_vldoor_t(door);
	    door->sector = &sectors[SaveIndex(door->sector, numsectors)];
	    door->sector->specialdata = door;
	    door->thinker.function.acp1 = (actionf_p1)T_VerticalDoor;
	    P_AddThinker (&door->thinker);
	    break;
				
	  case tc_floor:
	    PADSAVEP();
	    floor = Z_Malloc (sizeof(*floor), PU_LEVEL, NULL);
	    SaveRead_floormove_t(floor);
	    floor->sector = &sectors[SaveIndex(floor->sector, numsectors)];
	    floor->sector->specialdata = floor;
	    floor->thinker.function.acp1 = (actionf_p1)T_MoveFloor;
	    P_AddThinker (&floor->thinker);
	    break;
				
	  case tc_plat:
	    PADSAVEP();
	    plat = Z_Malloc (sizeof(*plat), PU_LEVEL, NULL);
	    SaveRead_plat_t(plat);
	    plat->sector = &sectors[SaveIndex(plat->sector, numsectors)];
	    plat->sector->specialdata = plat;

	    if (plat->thinker.function.acp1)
		plat->thinker.function.acp1 = (actionf_p1)T_PlatRaise;

	    P_AddThinker (&plat->thinker);
	    P_AddActivePlat(plat);
	    break;
				
	  case tc_flash:
	    PADSAVEP();
	    flash = Z_Malloc (sizeof(*flash), PU_LEVEL, NULL);
	    SaveRead_lightflash_t(flash);
	    flash->sector = &sectors[SaveIndex(flash->sector, numsectors)];
	    flash->thinker.function.acp1 = (actionf_p1)T_LightFlash;
	    P_AddThinker (&flash->thinker);
	    break;
				
	  case tc_strobe:
	    PADSAVEP();
	    strobe = Z_Malloc (sizeof(*strobe), PU_LEVEL, NULL);
	    SaveRead_strobe_t(strobe);
	    strobe->sector = &sectors[SaveIndex(strobe->sector, numsectors)];
	    strobe->thinker.function.acp1 = (actionf_p1)T_StrobeFlash;
	    P_AddThinker (&strobe->thinker);
	    break;
				
	  case tc_glow:
	    PADSAVEP();
	    glow = Z_Malloc (sizeof(*glow), PU_LEVEL, NULL);
	    SaveRead_glow_t(glow);
	    glow->sector = &sectors[SaveIndex(glow->sector, numsectors)];
	    glow->thinker.function.acp1 = (actionf_p1)T_Glow;
	    P_AddThinker (&glow->thinker);
	    break;
				
	  default:
	    I_Error ("P_UnarchiveSpecials:Unknown tclass %i "
		     "in savegame",tclass);
	}
	
    }

}

