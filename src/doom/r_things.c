// Native macOS port: modified 2026-09-30; updated 2026-10-01; GPL-2.0.
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
//	Refresh of things, i.e. objects represented by sprites.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_things.c,v 1.5 1997/02/03 16:47:56 b1 Exp $";


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>


#include "doomdef.h"
#include "Native.h"
#include "m_swap.h"

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "r_local.h"

#include "doomstat.h"
#include <dispatch/dispatch.h>



#define MINZ				(FRACUNIT*4)
#define BASEYCENTER			100

//void R_DrawColumn (void);
//void R_DrawFuzzColumn (void);



typedef struct
{
    int		x1;
    int		x2;
	
    int		column;
    int		topclip;
    int		bottomclip;

} maskdraw_t;



//
// Sprite rotation 0 is facing the viewer,
//  rotation 1 is one angle turn CLOCKWISE around the axis.
// This is not the same as the angle,
//  which increases counter clockwise (protractor).
// There was a lot of stuff grabbed wrong, so I changed it...
//
fixed_t		pspritescale;
fixed_t		pspriteiscale;

lighttable_t**	spritelights;

// constant arrays
//  used for psprite clipping and initializing clipping
short		negonearray[NATIVE_MAX_WIDTH];
short		screenheightarray[NATIVE_MAX_WIDTH];


//
// INITIALIZATION FUNCTIONS
//

// variables used to look up
//  and range check thing_t sprites patches
spritedef_t*	sprites;
int		numsprites;

spriteframe_t	sprtemp[29];
int		maxframe;
char*		spritename;




//
// R_InstallSpriteLump
// Local function for R_InitSprites.
//
void
R_InstallSpriteLump
( int		lump,
  unsigned	frame,
  unsigned	rotation,
  boolean	flipped )
{
    int		r;
	
    if (frame >= 29 || rotation > 8)
	I_Error("R_InstallSpriteLump: "
		"Bad frame characters in lump %i", lump);
	
    if ((int)frame > maxframe)
	maxframe = frame;
		
    if (rotation == 0)
    {
	// the lump should be used for all rotations
	if (sprtemp[frame].rotate == false)
	    I_Error ("R_InitSprites: Sprite %s frame %c has "
		     "multip rot=0 lump", spritename, 'A'+frame);

	if (sprtemp[frame].rotate == true)
	    I_Error ("R_InitSprites: Sprite %s frame %c has rotations "
		     "and a rot=0 lump", spritename, 'A'+frame);
			
	sprtemp[frame].rotate = false;
	for (r=0 ; r<8 ; r++)
	{
	    sprtemp[frame].lump[r] = lump - firstspritelump;
	    sprtemp[frame].flip[r] = (byte)flipped;
	}
	return;
    }
	
    // the lump is only used for one rotation
    if (sprtemp[frame].rotate == false)
	I_Error ("R_InitSprites: Sprite %s frame %c has rotations "
		 "and a rot=0 lump", spritename, 'A'+frame);
		
    sprtemp[frame].rotate = true;

    // make 0 based
    rotation--;		
    if (sprtemp[frame].lump[rotation] != -1)
	I_Error ("R_InitSprites: Sprite %s : %c : %c "
		 "has two lumps mapped to it",
		 spritename, 'A'+frame, '1'+rotation);
		
    sprtemp[frame].lump[rotation] = lump - firstspritelump;
    sprtemp[frame].flip[rotation] = (byte)flipped;
}




//
// R_InitSpriteDefs
// Pass a null terminated list of sprite names
//  (4 chars exactly) to be used.
// Builds the sprite rotation matrixes to account
//  for horizontally flipped sprites.
// Will report an error if the lumps are inconsistant. 
// Only called at startup.
//
// Sprite lump names are 4 characters for the actor,
//  a letter for the frame, and a number for the rotation.
// A sprite that is flippable will have an additional
//  letter/number appended.
// The rotation character can be 0 to signify no rotations.
//
void R_InitSpriteDefs (char** namelist) 
{ 
    char**	check;
    int		i;
    int		l;
    int		intname;
    int		frame;
    int		rotation;
    int		start;
    int		end;
    int		patched;
		
    // count the number of sprite names
    check = namelist;
    while (*check != NULL)
	check++;

    numsprites = check-namelist;
	
    if (!numsprites)
	return;
		
    sprites = Z_Malloc(numsprites *sizeof(*sprites), PU_STATIC, NULL);
	
    start = firstspritelump-1;
    end = lastspritelump+1;
	
    // scan all the lump names for each of the names,
    //  noting the highest frame letter.
    // Just compare 4 characters as ints
    for (i=0 ; i<numsprites ; i++)
    {
	spritename = namelist[i];
	memset (sprtemp,-1, sizeof(sprtemp));
		
	maxframe = -1;
	intname = *(int *)namelist[i];
	
	// scan the lumps,
	//  filling in the frames for whatever is found
	for (l=start+1 ; l<end ; l++)
	{
	    if (*(int *)lumpinfo[l].name == intname)
	    {
		frame = lumpinfo[l].name[4] - 'A';
		rotation = lumpinfo[l].name[5] - '0';

		if (modifiedgame)
		    patched = W_GetNumForName (lumpinfo[l].name);
		else
		    patched = l;

		R_InstallSpriteLump (patched, frame, rotation, false);

		if (lumpinfo[l].name[6])
		{
		    frame = lumpinfo[l].name[6] - 'A';
		    rotation = lumpinfo[l].name[7] - '0';
		    R_InstallSpriteLump (l, frame, rotation, true);
		}
	    }
	}
	
	// check the frames that were found for completeness
	if (maxframe == -1)
	{
	    sprites[i].numframes = 0;
	    continue;
	}
		
	maxframe++;
	
	for (frame = 0 ; frame < maxframe ; frame++)
	{
	    switch ((int)sprtemp[frame].rotate)
	    {
	      case -1:
		// no rotations were found for that frame at all
		I_Error ("R_InitSprites: No patches found "
			 "for %s frame %c", namelist[i], frame+'A');
		break;
		
	      case 0:
		// only the first rotation is needed
		break;
			
	      case 1:
		// must have all 8 frames
		for (rotation=0 ; rotation<8 ; rotation++)
		    if (sprtemp[frame].lump[rotation] == -1)
			I_Error ("R_InitSprites: Sprite %s frame %c "
				 "is missing rotations",
				 namelist[i], frame+'A');
		break;
	    }
	}
	
	// allocate space for the frames present and copy sprtemp to it
	sprites[i].numframes = maxframe;
	sprites[i].spriteframes = 
	    Z_Malloc (maxframe * sizeof(spriteframe_t), PU_STATIC, NULL);
	memcpy (sprites[i].spriteframes, sprtemp, maxframe*sizeof(spriteframe_t));
    }

}




//
// GAME FUNCTIONS
//
vissprite_t	vissprites[MAXVISSPRITES];
vissprite_t*	vissprite_p;
int		newvissprite;



//
// R_InitSprites
// Called at program start.
//
void R_InitSprites (char** namelist)
{
    int		i;
	
    for (i=0 ; i<NATIVE_MAX_WIDTH ; i++)
    {
	negonearray[i] = -1;
    }
	
    R_InitSpriteDefs (namelist);
}



//
// R_ClearSprites
// Called at frame start.
//
void R_ClearSprites (void)
{
    vissprite_p = vissprites;
}


//
// R_NewVisSprite
//
vissprite_t	overflowsprite;

vissprite_t* R_NewVisSprite (void)
{
    if (vissprite_p == &vissprites[MAXVISSPRITES])
	return &overflowsprite;
    
    vissprite_p++;
    return vissprite_p-1;
}



//
// R_DrawMaskedColumn
// Used for sprites and masked mid textures.
// Masked means: partly transparent, i.e. stored
//  in posts/runs of opaque pixels.
//
short*		mfloorclip;
short*		mceilingclip;

fixed_t		spryscale;
fixed_t		sprtopscreen;

void R_DrawMaskedColumn (column_t* column)
{
    int		topscreen;
    int 	bottomscreen;
    fixed_t	basetexturemid;
	
    basetexturemid = dc_texturemid;
	
    for ( ; column->topdelta != 0xff ; ) 
    {
	// calculate unclipped screen coordinates
	//  for post
	topscreen = sprtopscreen + spryscale*column->topdelta;
	bottomscreen = topscreen + spryscale*column->length;

	dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
	dc_yh = (bottomscreen-1)>>FRACBITS;
		
	if (dc_yh >= mfloorclip[dc_x])
	    dc_yh = mfloorclip[dc_x]-1;
	if (dc_yl <= mceilingclip[dc_x])
	    dc_yl = mceilingclip[dc_x]+1;

	if (dc_yl <= dc_yh)
	{
	    dc_source = (byte *)column + 3;
	    dc_postlength = column->length;
	    dc_texturemid = basetexturemid - (column->topdelta<<FRACBITS);
	    // dc_source = (byte *)column + 3 - column->topdelta;

	    // Drawn by either R_DrawColumn
	    //  or (SHADOW) R_DrawFuzzColumn.
	    colfunc ();	
	}
	column = (column_t *)(  (byte *)column + column->length + 4);
    }
	
    dc_texturemid = basetexturemid;
}



//
// R_DrawParticle
// Cosmetic blood ellipse, clipped like any sprite column.
//
extern byte*	ylookup[];
extern int	columnofs[];

// One particle's setup, shared read-only by the workers drawing its columns.
typedef struct
{
    vissprite_t*	vis;
    uint16_t*		light;
    byte*		base;
    const byte*		map;
    const byte*		next;
    unsigned		blend, seed;
    double		cx, cy, rx, up, down, cell;
    int			piece, yl, yh;
    boolean		chunk;
    const short*	floorclip;	// this particle's clips, from vis->x1
    const short*	ceilingclip;
} particle_t;

// Draws columns x1..x2 of a particle row by row, so each row's writes are
// contiguous in memory; neighbouring pixels in one world cell share a
// single shading decision.
enum { PARTICLEBAND = 64, PARTICLEPARALLEL = 16384 };
static void R_DrawParticleSpan (const particle_t* p, int x1, int x2)
{
    vissprite_t* vis = p->vis;
    const byte* map = p->map;
    double cx = p->cx, cy = p->cy, rx = p->rx, up = p->up, down = p->down, cell = p->cell;
    unsigned seed = p->seed;
    int piece = p->piece, x, y, ymin = INT_MAX, ymax = INT_MIN;
    boolean chunk = p->chunk;
    int qxs[PARTICLEBAND], tops[PARTICLEBAND], bottoms[PARTICLEBAND];
    double oxs[PARTICLEBAND];

    for (x = x1 ; x <= x2 ; x++)
    {
	// Sample on the half-unit world texel grid, like the stains.
	int c = x-x1, qx = (int)floor((x+0.5-cx)/cell), top = p->yl, bottom = p->yh;
	double ox = (qx+0.5)*cell/rx;
	if (bottom >= p->floorclip[x-vis->x1])
	    bottom = p->floorclip[x-vis->x1]-1;
	if (top <= p->ceilingclip[x-vis->x1])
	    top = p->ceilingclip[x-vis->x1]+1;
	if (ox*ox >= 1)
	    bottom = top-1;
	qxs[c] = qx; oxs[c] = ox; tops[c] = top; bottoms[c] = bottom;
	if (top <= bottom)
	{
	    if (top < ymin) ymin = top;
	    if (bottom > ymax) ymax = bottom;
	}
    }
    for (y = ymin ; y <= ymax ; y++)
    {
	int qy = (int)floor((y+0.5-cy)/cell), lastqx = INT_MIN, skip = 1, color = 0;
	double oy = (qy+0.5)*cell;
	byte* row = ylookup[y];
	oy /= oy < 0 ? up : down;
	for (x = x1 ; x <= x2 ; x++)
	{
	    int c = x-x1;
	    byte* dest;
	    if (y < tops[c] || y > bottoms[c])
		continue;
	    if (qxs[c] != lastqx)
	    {
		double ox = oxs[c], r = ox*ox+oy*oy;
		unsigned h;
		lastqx = qxs[c]; skip = 1;
		if (r >= 1)
		    continue;
		h = (seed ^ (unsigned)lastqx*73856093u ^ (unsigned)qy*19349663u)*2246822519u;
		h ^= h>>15;
		// Ragged flesh edges, mottled tissue and a darker underside; bone
		// is clean-edged and pale; a severed part shows its red wound end.
		if (chunk && piece != 2 && r > 0.45 && (h&3) == 0)
		    continue;
		if (piece == 2)
		    color = vis->pcolor + (oy > 0.3 ? 2 : 0) + (int)(h>>8)%2;
		else if ((piece == 3 || piece == 4) && ox*(piece == 3 ? 1 : -1) > 0.55)
		    color = 40 + (int)(h>>8)%3;
		else
		    color = vis->pcolor + (chunk ? (int)(h>>8)%3 + (oy > 0.2 ? 2 : 0) : (r > 0.5 ? 1 : 0));
		skip = 0;
	    }
	    if (skip)
		continue;
	    dest = row + columnofs[x];
	    *dest = map[color];
	    if (p->light)
		p->light[dest-p->base] = p->blend ? (p->next[color]|p->blend) : 0;
	}
    }
}

// Consecutive particles in back-to-front order are queued with their own
// clip arrays and drawn together: column bands go to all cores, and each
// band replays the queue in order, so overlaps resolve exactly as before.
// Anything else that draws into the view flushes the queue first.
typedef struct { particle_t p; size_t clip; } queuedparticle_t;
static queuedparticle_t* particlequeue;
static int particlecount, particlecap, queuex1, queuex2;
static long queuearea;
static short* particleclips;
static size_t clipused, clipcap;
static void R_DrawParticleQueueBand (void* context, size_t band)
{
    int x1 = queuex1 + (int)band*PARTICLEBAND, x2 = x1+PARTICLEBAND-1, i;
    if (x2 > queuex2)
	x2 = queuex2;
    for (i = 0 ; i < particlecount ; i++)
    {
	const particle_t* p = &particlequeue[i].p;
	int a = p->vis->x1 > x1 ? p->vis->x1 : x1, b = p->vis->x2 < x2 ? p->vis->x2 : x2;
	if (a <= b)
	    R_DrawParticleSpan (p, a, b);
    }
}
void R_FlushParticles (void)
{
    size_t bands, band;
    int i;
    if (!particlecount)
	return;
    for (i = 0 ; i < particlecount ; i++)
    {
	queuedparticle_t* q = &particlequeue[i];
	int width = q->p.vis->x2-q->p.vis->x1+1;
	q->p.floorclip = particleclips+q->clip;
	q->p.ceilingclip = particleclips+q->clip+width;
    }
    bands = (size_t)(queuex2-queuex1+PARTICLEBAND)/PARTICLEBAND;
    if (bands > 1 && queuearea >= PARTICLEPARALLEL)
	dispatch_apply_f(bands, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), NULL, R_DrawParticleQueueBand);
    else
	for (band = 0 ; band < bands ; band++)
	    R_DrawParticleQueueBand (NULL, band);
    particlecount = 0; clipused = 0; queuearea = 0;
}

static void R_DrawParticle (vissprite_t* vis)
{
    particle_t	p;
    int		width = vis->x2-vis->x1+1;
    queuedparticle_t* q;

    p.vis = vis;
    p.light = R_NativeLightPixels();
    p.base = Native_RenderPixels();
    p.map = vis->colormap ? vis->colormap : colormaps;
    p.next = vis->colormap_next;
    p.blend = p.next ? (vis->lightweight>>1)<<8 : 0;
    p.cx = vis->pcx/65536.0; p.cy = vis->pcy/65536.0;
    p.rx = vis->prx/65536.0; p.up = vis->pryup/65536.0; p.down = vis->prydown/65536.0;
    p.cell = vis->pcell/65536.0;
    p.seed = (unsigned)vis->gx*2654435761u ^ (unsigned)vis->gy;
    p.piece = vis->mobjflags;	// gore::Piece; 0 is a blood drop
    p.chunk = p.piece != 0;
    p.floorclip = p.ceilingclip = NULL;

    if (p.cell < 1)
	p.cell = 1;
    if (p.rx < p.cell*0.5)
	p.rx = p.cell*0.5;
    if (p.up < p.cell*0.5)
	p.up = p.cell*0.5;
    if (p.down < p.up)
	p.down = p.up;
    p.yl = (int)floor(p.cy-p.up);
    p.yh = (int)ceil(p.cy+p.down)-1;
    if (width <= 0)
	return;
    if (particlecount == particlecap)
    {
	particlecap = particlecap ? particlecap*2 : 256;
	particlequeue = realloc(particlequeue, sizeof(*particlequeue)*particlecap);
	if (!particlequeue) I_Error("R_DrawParticle: out of memory");
    }
    if (clipused+2*(size_t)width > clipcap)
    {
	while (clipused+2*(size_t)width > clipcap)
	    clipcap = clipcap ? clipcap*2 : 65536;
	particleclips = realloc(particleclips, sizeof(short)*clipcap);
	if (!particleclips) I_Error("R_DrawParticle: out of memory");
    }
    memcpy(particleclips+clipused, mfloorclip+vis->x1, sizeof(short)*width);
    memcpy(particleclips+clipused+width, mceilingclip+vis->x1, sizeof(short)*width);
    q = &particlequeue[particlecount++];
    q->p = p; q->clip = clipused; clipused += 2*(size_t)width;
    if (particlecount == 1 || vis->x1 < queuex1) queuex1 = vis->x1;
    if (particlecount == 1 || vis->x2 > queuex2) queuex2 = vis->x2;
    queuearea += (long)width*(p.yh-p.yl+1);
}


//
// R_ProjectParticle
// Same view transform and diminished light as R_ProjectSprite.
//
static void R_ProjectParticle (const native_particle_t* p)
{
    fixed_t	tr_x = p->x - viewx, tr_y = p->y - viewy;
    fixed_t	tz = FixedMul(tr_x,viewcos) + FixedMul(tr_y,viewsin);
    fixed_t	tx, xscale, cx, rx, ry;
    int		x1, x2, index;
    vissprite_t* vis;

    if (tz < 8*FRACUNIT)
	return;
    tx = FixedMul(tr_x,viewsin) - FixedMul(tr_y,viewcos);
    if (abs(tx) > (tz<<2))
	return;
    xscale = FixedDiv(projection, tz);
    cx = centerxfrac + FixedMul(tx, xscale);
    rx = FixedMul(p->radius, xscale);
    ry = rx;
    if (p->chunk == 3)	// a limb lies long and low
    {
	ry = rx*11/20;
	rx = rx*19/10;
    }
    x1 = (cx - rx) >> FRACBITS;
    x2 = (cx + rx) >> FRACBITS;
    if (x1 >= viewwidth || x2 < 0)
	return;
    vis = R_NewVisSprite ();
    if (vis == &overflowsprite)
	return;
    memset (vis, 0, sizeof(*vis));
    vis->particle = 1;
    vis->mobjflags = p->chunk;
    vis->pcolor = p->color;
    vis->scale = xscale<<detailshift;
    vis->gx = p->x;
    vis->gy = p->y;
    vis->gz = p->z - p->radius - p->stretch;
    vis->gzt = p->z + p->radius;
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;
    vis->pcx = cx;
    vis->pcy = centeryfrac - FixedMul(p->z - viewz, xscale);
    vis->prx = rx;
    vis->pryup = ry;
    vis->prydown = ry + FixedMul(p->stretch, xscale);
    vis->pcell = FixedMul(FRACUNIT/2, xscale);
    if (fixedcolormap)
	vis->colormap = fixedcolormap;
    else
    {
	index = (native_render_height==200?(int64_t)xscale*SCREENWIDTH/native_render_width:(int64_t)xscale*(SCREENWIDTH/2)*FRACUNIT/projection)>>(LIGHTSCALESHIFT-detailshift);
	if (index >= MAXLIGHTSCALE)
	    index = MAXLIGHTSCALE-1;
	dc_colormap = spritelights[index];
	R_ColumnLighting(spritelights,(fixed_t)((int64_t)xscale*(SCREENWIDTH/2)*FRACUNIT/projection));
	vis->colormap = dc_colormap;
	vis->colormap_next = dc_colormap_next;
	vis->lightweight = dc_lightweight;
    }
}


//
// R_DrawVisSprite
//  mfloorclip and mceilingclip should also be set.
//
void
R_DrawVisSprite
( vissprite_t*		vis,
  int			x1,
  int			x2 )
{
    column_t*		column;
    int			texturecolumn;
    fixed_t		frac;
    patch_t*		patch;
    boolean		relief;
	
	
    if (vis->particle)
    {
	R_DrawParticle (vis);
	return;
    }
    R_FlushParticles ();
    patch = W_CacheLumpNum (vis->patch+firstspritelump, PU_CACHE);

    dc_colormap = vis->colormap;
    dc_colormap_next=vis->colormap_next;
    dc_lightweight=vis->lightweight;
    
    if (!dc_colormap)
    {
	// NULL colormap = shadow draw
	colfunc = fuzzcolfunc;
    }
    else if (vis->mobjflags & MF_TRANSLATION)
    {
	colfunc = R_DrawTranslatedColumn;
	dc_translation = translationtables - 256 +
	    ( (vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT-8) );
    }
	
    dc_iscale = abs(vis->xiscale)>>detailshift;
    dc_texturemid = vis->texturemid;
    frac = vis->startfrac;
    spryscale = vis->scale;
    sprtopscreen = centeryfrac - FixedMul(dc_texturemid,spryscale);
	
    relief = colfunc == basecolfunc && !fixedcolormap && R_NativeLightPixels()
	&& !(vis->colormap == colormaps && !vis->lightweight);
    for (dc_x=vis->x1 ; dc_x<=vis->x2 ; dc_x++, frac += vis->xiscale)
    {
	texturecolumn = frac>>FRACBITS;
	// Native resolutions accumulate the 16.16 step over thousands of
	// columns; the last one can round just outside the patch.
	if (texturecolumn < 0)
	    texturecolumn = 0;
	else if (texturecolumn >= SHORT(patch->width))
	    texturecolumn = SHORT(patch->width)-1;
	column = (column_t *) ((byte *)patch +
			       LONG(patch->columnofs[texturecolumn]));
	// Native depth: shade the sprite as a rounded body, darker toward
	// the edges of its art (fullbright frames keep their light).
	if (relief)
	{
	    double t = (texturecolumn+0.5)/SHORT(patch->width)*2-1;
	    dc_spriteround = 1.4*(1-sqrt(1-t*t));
	}
	R_DrawMaskedColumn (column);
    }

    dc_spriteround = -1;
    dc_postlength = 0;
    colfunc = basecolfunc;
}



//
// R_ProjectSprite
// Generates a vissprite for a thing
//  if it might be visible.
//
void R_ProjectSprite (mobj_t* thing)
{
    fixed_t		tr_x;
    fixed_t		tr_y;
    
    fixed_t		gxt;
    fixed_t		gyt;
    
    fixed_t		tx;
    fixed_t		tz;

    fixed_t		xscale;
    
    int			x1;
    int			x2;

    spritedef_t*	sprdef;
    spriteframe_t*	sprframe;
    int			lump;
    
    unsigned		rot;
    boolean		flip;
    
    int			index;

    vissprite_t*	vis;
    
    angle_t		ang;
    fixed_t		iscale;
    
    fixed_t lift = Native_BodyLift(thing);
    int32_t jolt_x, jolt_y;
    Native_BodyFlinch(thing, &jolt_x, &jolt_y);
    thing = Native_RenderMobj(thing);

    // transform the origin point
    tr_x = thing->x + jolt_x - viewx;
    tr_y = thing->y + jolt_y - viewy;
	
    gxt = FixedMul(tr_x,viewcos); 
    gyt = -FixedMul(tr_y,viewsin);
    
    tz = gxt-gyt; 

    // thing is behind view plane?
    if (tz < MINZ)
	return;
    
    xscale = FixedDiv(projection, tz);
	
    gxt = -FixedMul(tr_x,viewsin); 
    gyt = FixedMul(tr_y,viewcos); 
    tx = -(gyt+gxt); 

    // too far off the side?
    if (abs(tx)>(tz<<2))
	return;
    
    // decide which patch to use for sprite relative to player
#ifdef RANGECHECK
    if ((unsigned)thing->sprite >= numsprites)
	I_Error ("R_ProjectSprite: invalid sprite number %i ",
		 thing->sprite);
#endif
    sprdef = &sprites[thing->sprite];
#ifdef RANGECHECK
    if ( (thing->frame&FF_FRAMEMASK) >= sprdef->numframes )
	I_Error ("R_ProjectSprite: invalid sprite frame %i : %i ",
		 thing->sprite, thing->frame);
#endif
    sprframe = &sprdef->spriteframes[ thing->frame & FF_FRAMEMASK];

    if (sprframe->rotate)
    {
	// choose a different rotation based on player view
	ang = R_PointToAngle (thing->x, thing->y);
	rot = (ang-thing->angle+(unsigned)(ANG45/2)*9)>>29;
	lump = sprframe->lump[rot];
	flip = (boolean)sprframe->flip[rot];
    }
    else
    {
	// use single rotation for all views
	lump = sprframe->lump[0];
	flip = (boolean)sprframe->flip[0];
    }
    
    // calculate edges of the shape
    tx -= spriteoffset[lump];	
    x1 = (centerxfrac + FixedMul (tx,xscale) ) >>FRACBITS;

    // off the right side?
    if (x1 > viewwidth)
	return;
    
    tx +=  spritewidth[lump];
    x2 = ((centerxfrac + FixedMul (tx,xscale) ) >>FRACBITS) - 1;

    // off the left side
    if (x2 < 0)
	return;
    
    // store information in a vissprite
    vis = R_NewVisSprite ();
    memset (vis, 0, sizeof(*vis));
    vis->mobjflags = thing->flags;
    vis->scale = xscale<<detailshift;
    vis->gx = thing->x + jolt_x;
    vis->gy = thing->y + jolt_y;
    vis->gz = thing->z + lift;
    vis->gzt = thing->z + lift + spritetopoffset[lump];
    vis->texturemid = vis->gzt - viewz;
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;	
    iscale = FixedDiv (FRACUNIT, xscale);

    if (flip)
    {
	vis->startfrac = spritewidth[lump]-1;
	vis->xiscale = -iscale;
    }
    else
    {
	vis->startfrac = 0;
	vis->xiscale = iscale;
    }

    if (vis->x1 > x1)
	vis->startfrac += vis->xiscale*(vis->x1-x1);
    vis->patch = lump;
    
    // get light level
    if (thing->flags & MF_SHADOW)
    {
	// shadow draw
	vis->colormap = NULL;
    }
    else if (fixedcolormap)
    {
	// fixed map
	vis->colormap = fixedcolormap;
    }
    else if (thing->frame & FF_FULLBRIGHT)
    {
	// full bright
	vis->colormap = colormaps;
    }
    
    else
    {
	// diminished light
	index = (native_render_height==200?(int64_t)xscale*SCREENWIDTH/native_render_width:(int64_t)xscale*(SCREENWIDTH/2)*FRACUNIT/projection)>>(LIGHTSCALESHIFT-detailshift);

	if (index >= MAXLIGHTSCALE) 
	    index = MAXLIGHTSCALE-1;

	vis->colormap = spritelights[index];
        dc_colormap=vis->colormap;
        R_ColumnLighting(spritelights,(fixed_t)((int64_t)xscale*(SCREENWIDTH/2)*FRACUNIT/projection));
        vis->colormap=dc_colormap; vis->colormap_next=dc_colormap_next; vis->lightweight=dc_lightweight;
    }	
}




//
// R_AddSprites
// During BSP traversal, this adds sprites by sector.
//
void R_AddSprites (sector_t* sec)
{
    mobj_t*		thing;
    int			lightnum;

    // BSP is traversed by subsector.
    // A sector might have been split into several
    //  subsectors during BSP building.
    // Thus we check whether its already added.
    if (sec->validcount == validcount)
	return;		

    // Well, now it will be done.
    sec->validcount = validcount;
	
    lightnum = (sec->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (lightnum < 0)		
	spritelights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	spritelights = scalelight[LIGHTLEVELS-1];
    else
	spritelights = scalelight[lightnum];

    // Handle all things in sector.
    for (thing = sec->thinglist ; thing ; thing = thing->snext)
	R_ProjectSprite (thing);
    Native_GoreSprites(sec,R_ProjectParticle);
}


//
// R_DrawPSprite
//
void R_DrawPSprite (pspdef_t* psp)
{
    fixed_t		tx;
    int			x1;
    int			x2;
    spritedef_t*	sprdef;
    spriteframe_t*	sprframe;
    int			lump;
    boolean		flip;
    vissprite_t*	vis;
    vissprite_t		avis = {0};
    
    // decide which patch to use
#ifdef RANGECHECK
    if ( (unsigned)psp->state->sprite >= numsprites)
	I_Error ("R_ProjectSprite: invalid sprite number %i ",
		 psp->state->sprite);
#endif
    sprdef = &sprites[psp->state->sprite];
#ifdef RANGECHECK
    if ( (psp->state->frame & FF_FRAMEMASK)  >= sprdef->numframes)
	I_Error ("R_ProjectSprite: invalid sprite frame %i : %i ",
		 psp->state->sprite, psp->state->frame);
#endif
    sprframe = &sprdef->spriteframes[ psp->state->frame & FF_FRAMEMASK ];

    lump = sprframe->lump[0];
    flip = (boolean)sprframe->flip[0];
    
    if (Native_Enhanced() && native_render_height!=200) {
        const byte *map=viewplayer->powers[pw_invisibility]>4*32 || (viewplayer->powers[pw_invisibility]&8)?NULL:
            (fixedcolormap?fixedcolormap:((psp->state->frame&FF_FULLBRIGHT)?colormaps:spritelights[MAXLIGHTSCALE-1]));
        Native_ViewmodelPatch(lump,flip,psp->sx,psp->sy,map);
        return;
    }

    // calculate edges of the shape
    tx = psp->sx-160*FRACUNIT;
	
    tx -= spriteoffset[lump];	
    x1 = (centerxfrac + FixedMul (tx,pspritescale) ) >>FRACBITS;

    // off the right side
    if (x1 > viewwidth)
	return;		

    tx +=  spritewidth[lump];
    x2 = ((centerxfrac + FixedMul (tx, pspritescale) ) >>FRACBITS) - 1;

    // off the left side
    if (x2 < 0)
	return;
    
    // store information in a vissprite
    vis = &avis;
    vis->mobjflags = 0;
    vis->texturemid = (BASEYCENTER<<FRACBITS)+FRACUNIT/2-(psp->sy-spritetopoffset[lump]);
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;	
    vis->scale = pspritescale<<detailshift;
    
    if (flip)
    {
	vis->xiscale = -pspriteiscale;
	vis->startfrac = spritewidth[lump]-1;
    }
    else
    {
	vis->xiscale = pspriteiscale;
	vis->startfrac = 0;
    }
    
    if (vis->x1 > x1)
	vis->startfrac += vis->xiscale*(vis->x1-x1);

    vis->patch = lump;

    if (viewplayer->powers[pw_invisibility] > 4*32
	|| viewplayer->powers[pw_invisibility] & 8)
    {
	// shadow draw
	vis->colormap = NULL;
    }
    else if (fixedcolormap)
    {
	// fixed color
	vis->colormap = fixedcolormap;
    }
    else if (psp->state->frame & FF_FULLBRIGHT)
    {
	// full bright
	vis->colormap = colormaps;
    }
    else
    {
	// local light
	vis->colormap = spritelights[MAXLIGHTSCALE-1];
    }
	
    R_DrawVisSprite (vis, vis->x1, vis->x2);
}



//
// R_DrawPlayerSprites
//
void R_DrawPlayerSprites (void)
{
    int		i;
    int		lightnum;
    pspdef_t*	psp;
    
    // get light level
    lightnum =
	(viewplayer->mo->subsector->sector->lightlevel >> LIGHTSEGSHIFT) 
	+extralight;

    if (lightnum < 0)		
	spritelights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	spritelights = scalelight[LIGHTLEVELS-1];
    else
	spritelights = scalelight[lightnum];
    
    // clip to screen bounds
    mfloorclip = screenheightarray;
    mceilingclip = negonearray;
    
    if (Native_Enhanced() && native_render_height!=200) Native_ViewmodelBegin(viewplayer);

    // add all active psprites
    for (i=0, psp=viewplayer->psprites;
	 i<NUMPSPRITES;
	 i++,psp++)
    {
	if (psp->state) {
            pspdef_t visual = *psp;
            Native_RenderPSprite(viewplayer,i,&visual.sx,&visual.sy);
            R_DrawPSprite (&visual);
        }
    }
}




//
// R_SortVisSprites
//
vissprite_t	vsprsortedhead;


static int R_CompareVisSprites (const void* a, const void* b)
{
    fixed_t left = (*(vissprite_t* const*)a)->scale, right = (*(vissprite_t* const*)b)->scale;
    return left < right ? -1 : left > right;
}

// Same order as the original selection sort (ascending scale, ties in
// projection order), in O(n log n): native views carry many particles.
void R_SortVisSprites (void)
{
    static vissprite_t*	order[MAXVISSPRITES];
    int			i;
    int			count;

    count = vissprite_p - vissprites;
    vsprsortedhead.next = vsprsortedhead.prev = &vsprsortedhead;
    if (!count)
	return;
    for (i=0 ; i<count ; i++)
	order[i] = &vissprites[i];
    mergesort (order, count, sizeof(order[0]), R_CompareVisSprites);
    for (i=0 ; i<count ; i++)
    {
	order[i]->prev = i ? order[i-1] : &vsprsortedhead;
	order[i]->next = i<count-1 ? order[i+1] : &vsprsortedhead;
    }
    vsprsortedhead.next = order[0];
    vsprsortedhead.prev = order[count-1];
}



//
// R_DrawSprite
//
void R_DrawSprite (vissprite_t* spr)
{
    drawseg_t*		ds;
    short		clipbot[NATIVE_MAX_WIDTH];
    short		cliptop[NATIVE_MAX_WIDTH];
    int			x;
    int			r1;
    int			r2;
    fixed_t		scale;
    fixed_t		lowscale;
    int			silhouette;
		
    for (x = spr->x1 ; x<=spr->x2 ; x++)
	clipbot[x] = cliptop[x] = -2;
    
    // Scan drawsegs from end to start for obscuring segs.
    // The first drawseg that has a greater scale
    //  is the clip seg.
    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)
    {
	// determine if the drawseg obscures the sprite
	if (ds->x1 > spr->x2
	    || ds->x2 < spr->x1
	    || (!ds->silhouette
		&& !ds->maskedtexturecol) )
	{
	    // does not cover sprite
	    continue;
	}
			
	r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
	r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;

	if (ds->scale1 > ds->scale2)
	{
	    lowscale = ds->scale2;
	    scale = ds->scale1;
	}
	else
	{
	    lowscale = ds->scale1;
	    scale = ds->scale2;
	}
		
	if (scale < spr->scale
	    || ( lowscale < spr->scale
		 && !R_PointOnSegSide (spr->gx, spr->gy, ds->curline) ) )
	{
	    // masked mid texture?
	    if (ds->maskedtexturecol)	
		R_RenderMaskedSegRange (ds, r1, r2);
	    // seg is behind sprite
	    continue;			
	}

	
	// clip this piece of the sprite
	silhouette = ds->silhouette;
	
	if (spr->gz >= ds->bsilheight)
	    silhouette &= ~SIL_BOTTOM;

	if (spr->gzt <= ds->tsilheight)
	    silhouette &= ~SIL_TOP;
			
	if (silhouette == 1)
	{
	    // bottom sil
	    for (x=r1 ; x<=r2 ; x++)
		if (clipbot[x] == -2)
		    clipbot[x] = ds->sprbottomclip[x];
	}
	else if (silhouette == 2)
	{
	    // top sil
	    for (x=r1 ; x<=r2 ; x++)
		if (cliptop[x] == -2)
		    cliptop[x] = ds->sprtopclip[x];
	}
	else if (silhouette == 3)
	{
	    // both
	    for (x=r1 ; x<=r2 ; x++)
	    {
		if (clipbot[x] == -2)
		    clipbot[x] = ds->sprbottomclip[x];
		if (cliptop[x] == -2)
		    cliptop[x] = ds->sprtopclip[x];
	    }
	}
		
    }
    
    // all clipping has been performed, so draw the sprite

    // check for unclipped columns
    for (x = spr->x1 ; x<=spr->x2 ; x++)
    {
	if (clipbot[x] == -2)		
	    clipbot[x] = viewheight;

	if (cliptop[x] == -2)
	    cliptop[x] = -1;
    }
		
    mfloorclip = clipbot;
    mceilingclip = cliptop;
    R_DrawVisSprite (spr, spr->x1, spr->x2);
}




//
// R_DrawMasked
//
void R_DrawMasked (void)
{
    vissprite_t*	spr;
    drawseg_t*		ds;
	
    R_SortVisSprites ();

    if (vissprite_p > vissprites)
    {
	// draw all vissprites back to front
	for (spr = vsprsortedhead.next ;
	     spr != &vsprsortedhead ;
	     spr=spr->next)
	{
	    
	    R_DrawSprite (spr);
	}
    }
    R_FlushParticles ();
    
    // render any remaining masked mid textures
    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)
	if (ds->maskedtexturecol)
	    R_RenderMaskedSegRange (ds, ds->x1, ds->x2);
    
    // draw the psprites on top of everything
    //  but does not draw on side views
    if (!viewangleoffset)		
	R_DrawPlayerSprites ();
}
