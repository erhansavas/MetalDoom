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
//	The actual span/column drawing functions.
//	Here find the main potential for optimization,
//	 e.g. inline assembly, different algorithms.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_draw.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";


#include "doomdef.h"
#include "Native.h"

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "r_local.h"

// Needs access to LFB (guess what).
#include "v_video.h"

// State.
#include "doomstat.h"
#include <math.h>
#include <limits.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>


// ?
#define MAXWIDTH			NATIVE_MAX_WIDTH
#define MAXHEIGHT			NATIVE_MAX_HEIGHT

// status bar height at bottom of screen
#define SBARHEIGHT		32

//
// All drawing to the view buffer is accomplished in this file.
// The other refresh files only know about ccordinates,
//  not the architecture of the frame buffer.
// Conveniently, the frame buffer is a linear one,
//  and we need only the base address,
//  and the total size == width*height*depth/8.,
//


byte*		viewimage; 
int		viewwidth;
int		scaledviewwidth;
int		viewheight;
int		viewwindowx;
int		viewwindowy; 
byte*		ylookup[MAXHEIGHT]; 
int		columnofs[MAXWIDTH]; 

// Color tables for different players,
//  translate a limited part to another
//  (color ramps used for  suit colors).
//
byte		translations[3][256];	
 
 


//
// R_DrawColumn
// Source is the top of the column to scale.
//
lighttable_t*		dc_colormap; 
lighttable_t *dc_colormap_next, *ds_colormap_next;
unsigned dc_lightweight, ds_lightweight;
// Native RGB lighting (2026-10-09): walls and flats store their unlit texel
// and a continuous shade (quarter colormap levels, ordered-dithered) so the
// shader lights them in RGB instead of stepping through palette colormaps.
double dc_shade = -1, ds_shade = -1, dc_spriteround = -1;
const byte *dc_sourceleft, *dc_sourceright;
int dc_wall = 0, dc_texheight = 0, dc_postlength = 0;
// Relief lighting: texture brightness as a height map, lit from above.
// Brightness per palette index (0..255), from PLAYPAL.
static int texlum[256];
static void R_InitRelief(void)
{
    static int built;
    if (built) return;
    const byte *pal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    for (int i = 0; i < 256; ++i)
	texlum[i] = (pal[i*3]*54 + pal[i*3+1]*183 + pal[i*3+2]*19) >> 8;
    built = 1;
}
// Shade offset (1/64 colormap levels) from a brightness gradient: a surface
// brightening downward faces up toward the light and reads lighter.
static inline int R_Relief(unsigned above, unsigned below, int strength)
{
    return -((texlum[below]-texlum[above])*strength) >> 8;
}
fixed_t dc_wallfloor, dc_wallceiling;
static const unsigned char bayer4[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
static inline void R_ShadePixel(byte *dest, uint16_t *light, unsigned texel, int shade64, int x, int y)
{
    int q = (shade64 + bayer4[y&3][x&3]) >> 4;
    if (q < 0) q = 0; else if (q > 127) q = 127;
    *dest = colormaps[(q>>2)*256+texel];
    *light = (uint16_t)(0x8000u | ((unsigned)q<<8) | texel);
}
static uint16_t *native_lightpixels;
static byte *native_lightbase;

//
// Native parallel drawing (2026-10-09). RGB-lit wall columns and flat spans
// never overlap one another, so during BSP traversal and R_DrawPlanes they
// are recorded and then drawn across all cores at once. Wall blood replays
// serially after its walls; sprites and masked geometry draw afterwards as
// before. Any zone allocation flushes first, so no pending command can
// reference a purged lump.
//
typedef struct { const byte *source; fixed_t xfrac, yfrac, xstep, ystep; int x1, x2, y, base64; } spancmd_t;
typedef struct { const byte *source, *left, *right; fixed_t iscale, texturemid, floorz, ceilz; int x, yl, yh, base64, height; } wallcmd_t;
typedef struct { const void *handle; lighttable_t *map, *next; fixed_t u, iscale; unsigned weight; int x, yl, yh; } gorecmd_t;
static spancmd_t *spancmds; static int spancount, spancap;
static wallcmd_t *wallcmds; static int wallcount, wallcap;
static gorecmd_t *gorecmds; static int gorecount, gorecap;
static int deferring;
static void R_RunWallRows(const wallcmd_t *c, int y0, int y1);
static void R_RunSpan(const spancmd_t *c);
#define R_PUSH(list, count, cap) \
    (count == cap ? (cap = cap ? cap*2 : 4096, list = realloc(list, sizeof(*list)*cap), \
		     list ? 0 : (I_Error("R_PUSH: out of memory"), 0)) : 0, &list[count++])
static void R_FlushDeferred(void);
void R_BeginNativeLighting(void) {
    uint16_t *buffer=Native_LightingBegin(native_render_width,native_render_height);
    native_lightpixels=native_render_height==200?NULL:buffer;
    native_lightbase=Native_RenderPixels();
    dc_lightweight=ds_lightweight=0;
    deferring=native_lightpixels!=NULL;
    if(native_lightpixels) R_InitRelief();
    z_beforealloc=R_FlushDeferred;
}
// Occlusion in 1/64 levels by whole units from the floor (40) and ceiling
// (28), quadratic falloff.
static int floorAO[40], ceilingAO[28];
static void R_InitWallOcclusion(void)
{
    static int built;
    if (built) return;
    for (int i = 0; i < 40; ++i) floorAO[i] = (int)(2.2*64*(1-i/40.0)*(1-i/40.0));
    for (int i = 0; i < 28; ++i) ceilingAO[i] = (int)(1.2*64*(1-i/28.0)*(1-i/28.0));
    built = 1;
}
// Wall: baked light at this column plus soft occlusion where the wall meets
// its floor and ceiling. Draws rows y0..y1 of the column; any row's texture
// position is exact (wrapping arithmetic), so a column may be drawn in pieces.
static void R_RunWallRows(const wallcmd_t *c, int y0, int y1)
{
    if (y0 < c->yl) y0 = c->yl;
    if (y1 > c->yh) y1 = c->yh;
    int count = y1 - y0;
    if (count < 0) return;
    byte *dest = ylookup[y0] + columnofs[c->x];
    uint16_t *light = native_lightpixels + (dest - native_lightbase);
    const int stride = native_render_width;
    fixed_t frac = c->texturemid + (y0-centery)*c->iscale;
    int64_t z = (int64_t)viewz - (int64_t)(y0-centery)*c->iscale;
    int y = y0;
    do {
	int row = (frac>>FRACBITS)&127;
	unsigned texel = c->source[row];
	int up = (int)((z-c->floorz)>>FRACBITS), down = (int)((c->ceilz-z)>>FRACBITS), ao = 0;
	if (row < c->height)
	    ao += R_Relief(c->source[row>0?row-1:row], c->source[row+1<c->height?row+1:row], 110)
		+ R_Relief(c->right[row], c->left[row], 70);
	if ((unsigned)up < 40) ao += floorAO[up];
	if ((unsigned)down < 28) ao += ceilingAO[down];
	R_ShadePixel(dest, light, texel, c->base64+ao, c->x, y);
	dest += stride; light += stride; frac += c->iscale; z -= c->iscale; ++y;
    } while (count--);
}
// Flat span with relief shading from the texture itself.
static void R_RunSpan(const spancmd_t *c)
{
    byte *dest = ylookup[c->y] + columnofs[c->x1];
    uint16_t *light = native_lightpixels + (dest - native_lightbase);
    fixed_t xfrac = c->xfrac, yfrac = c->yfrac;
    for (int x = c->x1; x <= c->x2; ++x) {
	int spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	int relief = R_Relief(c->source[(spot-64)&4095], c->source[(spot+64)&4095], 40)
		   + R_Relief(c->source[(spot&~63)|((spot-1)&63)], c->source[(spot&~63)|((spot+1)&63)], 40);
	R_ShadePixel(dest++, light++, c->source[spot], c->base64+relief, x, c->y);
	xfrac += c->xstep; yfrac += c->ystep;
    }
}
enum { R_CHUNK = 192, R_WALLROWS = 32 };
static void R_RunChunk(void *context, size_t index)
{
    size_t wallchunks = ((size_t)wallcount + R_CHUNK-1) / R_CHUNK;
    if (index < wallchunks) {
	// Walls never overlap, so a chunk is drawn in bands of rows across
	// all its columns: each band's pixels and light stay in L1 cache
	// instead of every row of a column landing on new cache lines.
	size_t begin = index*R_CHUNK, end = (index+1)*R_CHUNK < (size_t)wallcount ? (index+1)*R_CHUNK : (size_t)wallcount;
	int top = INT_MAX, bottom = INT_MIN;
	for (size_t i = begin; i < end; ++i) {
	    if (wallcmds[i].yl < top) top = wallcmds[i].yl;
	    if (wallcmds[i].yh > bottom) bottom = wallcmds[i].yh;
	}
	for (int band = top; band <= bottom; band += R_WALLROWS)
	    for (size_t i = begin; i < end; ++i) R_RunWallRows(&wallcmds[i], band, band+R_WALLROWS-1);
    } else {
	index -= wallchunks;
	size_t end = (index+1)*R_CHUNK < (size_t)spancount ? (index+1)*R_CHUNK : (size_t)spancount;
	for (size_t i = index*R_CHUNK; i < end; ++i) R_RunSpan(&spancmds[i]);
    }
}
enum { R_GORESTRIPS = 32 };
static void R_RunGoreStrip(void *context, size_t strip)
{
    int x1 = (int)(strip*native_render_width/R_GORESTRIPS), x2 = (int)((strip+1)*native_render_width/R_GORESTRIPS);
    for (int i = 0; i < gorecount; ++i) {
	const gorecmd_t *g = &gorecmds[i];
	if (g->x >= x1 && g->x < x2)
	    Native_GoreWallColumn(g->handle, g->u, g->yl, g->yh, g->x, g->iscale, g->map, g->next, g->weight);
    }
}
static void R_FlushDeferred(void)
{
    if (!wallcount && !spancount && !gorecount) return;
    R_InitWallOcclusion();
    size_t chunks = ((size_t)wallcount + R_CHUNK-1) / R_CHUNK + ((size_t)spancount + R_CHUNK-1) / R_CHUNK;
    if (chunks)
	dispatch_apply_f(chunks, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), NULL, R_RunChunk);
    wallcount = spancount = 0;
    // Wall blood draws over its finished walls. Each strip of screen columns
    // replays its own columns in recorded order, so overlaps keep their order.
    if (gorecount)
	dispatch_apply_f(R_GORESTRIPS, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), NULL, R_RunGoreStrip);
    gorecount = 0;
}
void R_EndDeferredDrawing(void) {
    R_FlushDeferred();
    deferring=0;
}
extern int fuzzoffset[];
void R_GoreColumn(const void *handle, fixed_t u, int yl, int yh)
{
    if (!deferring) { Native_GoreWallColumn(handle, u, yl, yh, dc_x, dc_iscale, dc_colormap, dc_colormap_next, dc_lightweight); return; }
    gorecmd_t *g = R_PUSH(gorecmds, gorecount, gorecap);
    g->handle = handle; g->map = dc_colormap; g->next = dc_colormap_next; g->weight = dc_lightweight;
    g->u = u; g->iscale = dc_iscale; g->x = dc_x; g->yl = yl; g->yh = yh;
}
uint16_t *R_NativeLightPixels(void) { return native_lightpixels; }
static unsigned R_NativeShade(double shade,lighttable_t **first,lighttable_t **next) {
    shade=fmax(0,fmin(NUMCOLORMAPS-1,shade));
    int level=(int)shade;
    *first=colormaps+level*256;
    *next=colormaps+(level+(level<NUMCOLORMAPS-1))*256;
    return (unsigned)lround((shade-level)*255);
}
void R_ColumnLighting(lighttable_t **table,fixed_t scale) {
    dc_lightweight=0; dc_shade=-1;
    if(!native_lightpixels || fixedcolormap) return;
    for(int i=0;i<LIGHTLEVELS;i++) if(table==scalelight[i]) {
        int start=((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
        dc_shade=fmax(0,start-scale/(double)(1<<LIGHTSCALESHIFT)/2);
        dc_lightweight=R_NativeShade(start-scale/(double)(1<<LIGHTSCALESHIFT)/2,&dc_colormap,&dc_colormap_next);
        return;
    }
}
// Thread-safe form of R_PlaneLighting for the parallel stain pass: returns
// the blend weight and both tables without touching drawing globals.
unsigned R_PlaneTables(lighttable_t **table,fixed_t distance,lighttable_t **map,lighttable_t **next) {
    unsigned index=(unsigned)distance>>LIGHTZSHIFT;
    *map=fixedcolormap?fixedcolormap:table[index<MAXLIGHTZ?index:MAXLIGHTZ-1]; *next=NULL;
    if(!native_lightpixels || fixedcolormap) return 0;
    for(int i=0;i<LIGHTLEVELS;i++) if(table==zlight[i]) {
        int start=((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
        return R_NativeShade(start-1280.0/(distance/(double)FRACUNIT+16),map,next);
    }
    return 0;
}
void R_PlaneLighting(lighttable_t **table,fixed_t distance) {
    ds_lightweight=0; ds_shade=-1;
    if(!native_lightpixels || fixedcolormap) return;
    for(int i=0;i<LIGHTLEVELS;i++) if(table==zlight[i]) {
        int start=((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
        ds_shade=fmax(0,start-1280.0/(distance/(double)FRACUNIT+16));
        ds_lightweight=R_NativeShade(start-1280.0/(distance/(double)FRACUNIT+16),&ds_colormap,&ds_colormap_next);
        return;
    }
}
int			dc_x; 
int			dc_yl; 
int			dc_yh; 
fixed_t			dc_iscale; 
fixed_t			dc_texturemid;

// first pixel in a column (possibly virtual) 
byte*			dc_source;		

// just for profiling 
int			dccount;

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
// 
void R_DrawColumn (void) 
{ 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 

    // Zero length, column does not exceed a pixel.
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= native_render_width
	|| dc_yl < 0
	|| dc_yh >= native_render_height) 
	I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x); 
#endif 

    // Framebuffer destination address.
    // Use ylookup LUT to avoid multiply with ScreenWidth.
    // Use columnofs LUT for subwindows? 
    dest = ylookup[dc_yl] + columnofs[dc_x];  

    // Determine scaling,
    //  which is the only mapping to be done.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    if(native_lightpixels && dc_wall && dc_shade>=0) {
        wallcmd_t local, *c = &local;
        c->source = dc_source; c->iscale = dc_iscale;
        c->left = dc_sourceleft ? dc_sourceleft : dc_source; c->right = dc_sourceright ? dc_sourceright : dc_source; c->texturemid = dc_texturemid;
        c->floorz = dc_wallfloor; c->ceilz = dc_wallceiling;
        c->x = dc_x; c->yl = dc_yl; c->yh = dc_yh; c->base64 = (int)(dc_shade*64);
        c->height = dc_texheight > 0 && dc_texheight <= 128 ? dc_texheight : 0;
        if (deferring) *R_PUSH(wallcmds, wallcount, wallcap) = local;
        else R_RunWallRows(c, c->yl, c->yh);
        return;
    }
    if(native_lightpixels && dc_spriteround >= 0 && dc_postlength > 0) {
        // Sprite with depth: rounded-body shading plus relief within this
        // post, through adjacent light tables.
        uint16_t *light=native_lightpixels+(dest-native_lightbase);
        const byte *source=dc_source;
        const int stride=native_render_width,length=dc_postlength;
        int level=(int)((dc_colormap-colormaps)/256);
        int base64=level*64+(int)(dc_lightweight*64/255)+(int)(dc_spriteround*64);
        do {
            int row=frac>>FRACBITS;
            if(row<0) row=0; else if(row>=length) row=length-1;
            unsigned texel=source[row];
            int s64=base64+R_Relief(source[row>0?row-1:row],source[row+1<length?row+1:row],90);
            if(s64<0) s64=0; else if(s64>(NUMCOLORMAPS-1)*64) s64=(NUMCOLORMAPS-1)*64;
            int l=s64>>6; unsigned w=(unsigned)(s64&63)*2;
            *dest=colormaps[l*256+texel];
            *light=w&&l<NUMCOLORMAPS-1?(uint16_t)(colormaps[(l+1)*256+texel]|(w<<8)):0;
            dest+=stride; light+=stride; frac+=fracstep;
        } while(count--);
        return;
    }
    if(native_lightpixels) {
        uint16_t *light=native_lightpixels+(dest-native_lightbase);
        const byte *source=dc_source,*map=dc_colormap,*next=dc_colormap_next;
        const int stride=native_render_width;
        const unsigned blend=(dc_lightweight>>1)<<8;
        do {
            unsigned texel=source[(frac>>FRACBITS)&127];
            *dest=map[texel];
            *light=blend?(next[texel]|blend):0;
            dest+=stride; light+=stride; frac+=fracstep;
        } while(count--);
        return;
    }

    // Inner loop that does the actual texture mapping,
    //  e.g. a DDA-lile scaling.
    // This is as fast as it gets.
    do 
    {
	// Re-map color indices from wall texture column
	//  using a lighting/special effects LUT.
	*dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	
	dest += native_render_width; 
	frac += fracstep;
	
    } while (count--); 
} 



// UNUSED.
// Loop unrolled.
#if 0
void R_DrawColumn (void) 
{ 
    int			count; 
    byte*		source;
    byte*		dest;
    byte*		colormap;
    
    unsigned		frac;
    unsigned		fracstep;
    unsigned		fracstep2;
    unsigned		fracstep3;
    unsigned		fracstep4;	 
 
    count = dc_yh - dc_yl + 1; 

    source = dc_source;
    colormap = dc_colormap;		 
    dest = ylookup[dc_yl] + columnofs[dc_x];  
	 
    fracstep = dc_iscale<<9; 
    frac = (dc_texturemid + (dc_yl-centery)*dc_iscale)<<9; 
 
    fracstep2 = fracstep+fracstep;
    fracstep3 = fracstep2+fracstep;
    fracstep4 = fracstep3+fracstep;
	
    while (count >= 8) 
    { 
	dest[0] = colormap[source[frac>>25]]; 
	dest[native_render_width] = colormap[source[(frac+fracstep)>>25]]; 
	dest[native_render_width*2] = colormap[source[(frac+fracstep2)>>25]]; 
	dest[native_render_width*3] = colormap[source[(frac+fracstep3)>>25]];
	
	frac += fracstep4; 

	dest[native_render_width*4] = colormap[source[frac>>25]]; 
	dest[native_render_width*5] = colormap[source[(frac+fracstep)>>25]]; 
	dest[native_render_width*6] = colormap[source[(frac+fracstep2)>>25]]; 
	dest[native_render_width*7] = colormap[source[(frac+fracstep3)>>25]]; 

	frac += fracstep4; 
	dest += native_render_width*8; 
	count -= 8;
    } 
	
    while (count > 0)
    { 
	*dest = colormap[source[frac>>25]]; 
	dest += native_render_width; 
	frac += fracstep; 
	count--;
    } 
}
#endif


void R_DrawColumnLow (void) 
{ 
    int			count; 
    byte*		dest; 
    byte*		dest2;
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= native_render_width
	|| dc_yl < 0
	|| dc_yh >= native_render_height)
    {
	
	I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
    //	dccount++; 
#endif 
    // Blocky mode, need to multiply by 2.
    dc_x <<= 1;
    
    dest = ylookup[dc_yl] + columnofs[dc_x];
    dest2 = ylookup[dc_yl] + columnofs[dc_x+1];
    
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep;
    
    do 
    {
	// Hack. Does not work corretly.
	*dest2 = *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	dest += native_render_width;
	dest2 += native_render_width;
	frac += fracstep; 

    } while (count--);
}


//
// Spectre/Invisibility.
//
#define FUZZTABLE		50 
#define FUZZOFF	(1)


int	fuzzoffset[FUZZTABLE] =
{
    FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF 
}; 

int	fuzzpos = 0; 


//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//
void R_DrawFuzzColumn (void) 
{ 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 

    // Adjust borders. Low... 
    if (!dc_yl) 
	dc_yl = 1;

    // .. and high.
    if (dc_yh == viewheight-1) 
	dc_yh = viewheight - 2; 
		 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 

    
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= native_render_width
	|| dc_yl < 0 || dc_yh >= native_render_height)
    {
	I_Error ("R_DrawFuzzColumn: %i to %i at %i",
		 dc_yl, dc_yh, dc_x);
    }
#endif


    // Keep till detailshift bug in blocky mode fixed,
    //  or blocky mode removed.
    /* WATCOM code 
    if (detailshift)
    {
	if (dc_x & 1)
	{
	    outpw (GC_INDEX,GC_READMAP+(2<<8) ); 
	    outp (SC_INDEX+1,12); 
	}
	else
	{
	    outpw (GC_INDEX,GC_READMAP); 
	    outp (SC_INDEX+1,3); 
	}
	dest = destview + dc_yl*80 + (dc_x>>1); 
    }
    else
    {
	outpw (GC_INDEX,GC_READMAP+((dc_x&3)<<8) ); 
	outp (SC_INDEX+1,1<<(dc_x&3)); 
	dest = destview + dc_yl*80 + (dc_x>>2); 
    }*/

    

    // Does not work with blocky mode.
    dest = ylookup[dc_yl] + columnofs[dc_x];

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Looks like an attempt at dithering,
    //  using the colormap #6 (of 0-31, a bit
    //  brighter than average).
    do 
    {
	// Lookup framebuffer, and retrieve
	//  a pixel that is either one column
	//  left or right of the current one.
	// Add index from colormap to index.
	*dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]*native_render_width]]; 
        if(native_lightpixels) native_lightpixels[dest-native_lightbase]=0;

	// Clamp table lookup index.
	if (++fuzzpos == FUZZTABLE) 
	    fuzzpos = 0;
	
	dest += native_render_width;

	frac += fracstep; 
    } while (count--); 
} 
 
  
 

//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//
byte*	dc_translation;
byte*	translationtables;

void R_DrawTranslatedColumn (void) 
{ 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= native_render_width
	|| dc_yl < 0
	|| dc_yh >= native_render_height)
    {
	I_Error ( "R_DrawColumn: %i to %i at %i",
		  dc_yl, dc_yh, dc_x);
    }
    
#endif 


    // WATCOM VGA specific.
    /* Keep for fixing.
    if (detailshift)
    {
	if (dc_x & 1)
	    outp (SC_INDEX+1,12); 
	else
	    outp (SC_INDEX+1,3);
	
	dest = destview + dc_yl*80 + (dc_x>>1); 
    }
    else
    {
	outp (SC_INDEX+1,1<<(dc_x&3)); 

	dest = destview + dc_yl*80 + (dc_x>>2); 
    }*/

    

    // FIXME. As above.
    dest = ylookup[dc_yl] + columnofs[dc_x]; 

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Here we do an additional index re-mapping.
    do 
    {
	// Translation tables are used
	//  to map certain colorramps to other ones,
	//  used with PLAY sprites.
	// Thus the "green" ramp of the player 0 sprite
	//  is mapped to gray, red, black/indigo. 
	*dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
        if(native_lightpixels) {
            unsigned texel=dc_translation[dc_source[frac>>FRACBITS]];
            native_lightpixels[dest-native_lightbase]=(dc_lightweight>>1)?(dc_colormap_next[texel]|((dc_lightweight>>1)<<8)):0;
        }
	dest += native_render_width;
	
	frac += fracstep; 
    } while (count--); 
} 




//
// R_InitTranslationTables
// Creates the translation tables to map
//  the green color ramp to gray, brown, red.
// Assumes a given structure of the PLAYPAL.
// Could be read from a lump instead.
//
void R_InitTranslationTables (void)
{
    int		i;
	
    translationtables = Z_Malloc (256*3+255, PU_STATIC, 0);
    translationtables = (byte *)(( (uintptr_t)translationtables + 255 )& ~255);
    
    // translate just the 16 green colors
    for (i=0 ; i<256 ; i++)
    {
	if (i >= 0x70 && i<= 0x7f)
	{
	    // map green ramp to gray, brown, red
	    translationtables[i] = 0x60 + (i&0xf);
	    translationtables [i+256] = 0x40 + (i&0xf);
	    translationtables [i+512] = 0x20 + (i&0xf);
	}
	else
	{
	    // Keep all other colors as is.
	    translationtables[i] = translationtables[i+256] 
		= translationtables[i+512] = i;
	}
    }
}




//
// R_DrawSpan 
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//
int			ds_y; 
int			ds_x1; 
int			ds_x2;

lighttable_t*		ds_colormap; 

fixed_t			ds_xfrac; 
fixed_t			ds_yfrac; 
fixed_t			ds_xstep; 
fixed_t			ds_ystep;

// start of a 64*64 tile image 
byte*			ds_source;	

// just for profiling
int			dscount;


//
// Draws the actual span.
void R_DrawSpan (void) 
{ 
    fixed_t		xfrac;
    fixed_t		yfrac; 
    byte*		dest; 
    int			count;
    int			spot; 
	 
#ifdef RANGECHECK 
    if (ds_x2 < ds_x1
	|| ds_x1<0
	|| ds_x2>=native_render_width  
	|| (unsigned)ds_y>native_render_height)
    {
	I_Error( "R_DrawSpan: %i to %i at %i",
		 ds_x1,ds_x2,ds_y);
    }
//	dscount++; 
#endif 

    
    xfrac = ds_xfrac; 
    yfrac = ds_yfrac; 
	 
    dest = ylookup[ds_y] + columnofs[ds_x1];

    // We do not check for zero spans here?
    count = ds_x2 - ds_x1; 

    if(native_lightpixels && ds_shade>=0) {
        spancmd_t local, *c = &local;
        c->source = ds_source; c->xfrac = xfrac; c->yfrac = yfrac; c->xstep = ds_xstep; c->ystep = ds_ystep;
        c->x1 = ds_x1; c->x2 = ds_x2; c->y = ds_y; c->base64 = (int)(ds_shade*64);
        if (deferring) *R_PUSH(spancmds, spancount, spancap) = local;
        else R_RunSpan(c);
        return;
    }
    if(native_lightpixels) {
        uint16_t *light=native_lightpixels+(dest-native_lightbase);
        const byte *source=ds_source,*map=ds_colormap,*next=ds_colormap_next;
        const fixed_t xstep=ds_xstep,ystep=ds_ystep;
        const unsigned blend=(ds_lightweight>>1)<<8;
        do {
            spot=((yfrac>>(16-6))&(63*64))+((xfrac>>16)&63);
            unsigned texel=source[spot];
            *dest++=map[texel];
            *light++=blend?(next[texel]|blend):0;
            xfrac+=xstep; yfrac+=ystep;
        } while(count--);
        return;
    }

    do 
    {
	// Current texture index in u,v.
	spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);

	// Lookup pixel from flat texture tile,
	//  re-index using light/colormap.
	*dest++ = ds_colormap[ds_source[spot]];

	// Next step in u,v.
	xfrac += ds_xstep; 
	yfrac += ds_ystep;
	
    } while (count--); 
} 



// UNUSED.
// Loop unrolled by 4.
#if 0
void R_DrawSpan (void) 
{ 
    unsigned	position, step;

    byte*	source;
    byte*	colormap;
    byte*	dest;
    
    unsigned	count;
    usingned	spot; 
    unsigned	value;
    unsigned	temp;
    unsigned	xtemp;
    unsigned	ytemp;
		
    position = ((ds_xfrac<<10)&0xffff0000) | ((ds_yfrac>>6)&0xffff);
    step = ((ds_xstep<<10)&0xffff0000) | ((ds_ystep>>6)&0xffff);
		
    source = ds_source;
    colormap = ds_colormap;
    dest = ylookup[ds_y] + columnofs[ds_x1];	 
    count = ds_x2 - ds_x1 + 1; 
	
    while (count >= 4) 
    { 
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[0] = colormap[source[spot]]; 

	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[1] = colormap[source[spot]];
	
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[2] = colormap[source[spot]];
	
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[3] = colormap[source[spot]]; 
		
	count -= 4;
	dest += 4;
    } 
    while (count > 0) 
    { 
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	*dest++ = colormap[source[spot]]; 
	count--;
    } 
} 
#endif


//
// Again..
//
void R_DrawSpanLow (void) 
{ 
    fixed_t		xfrac;
    fixed_t		yfrac; 
    byte*		dest; 
    int			count;
    int			spot; 
	 
#ifdef RANGECHECK 
    if (ds_x2 < ds_x1
	|| ds_x1<0
	|| ds_x2>=native_render_width  
	|| (unsigned)ds_y>native_render_height)
    {
	I_Error( "R_DrawSpan: %i to %i at %i",
		 ds_x1,ds_x2,ds_y);
    }
//	dscount++; 
#endif 
	 
    xfrac = ds_xfrac; 
    yfrac = ds_yfrac; 

    // Blocky mode, need to multiply by 2.
    ds_x1 <<= 1;
    ds_x2 <<= 1;
    
    dest = ylookup[ds_y] + columnofs[ds_x1];
  
    
    count = ds_x2 - ds_x1; 
    do 
    { 
	spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	// Lowres/blocky mode does it twice,
	//  while scale is adjusted appropriately.
	*dest++ = ds_colormap[ds_source[spot]]; 
	*dest++ = ds_colormap[ds_source[spot]];
	
	xfrac += ds_xstep; 
	yfrac += ds_ystep; 

    } while (count--); 
}

//
// R_InitBuffer 
// Creats lookup tables that avoid
//  multiplies and other hazzles
//  for getting the framebuffer address
//  of a pixel to draw.
//
void
R_InitBuffer
( int		width,
  int		height ) 
{ 
    int		i; 

    // Handle resize,
    //  e.g. smaller view windows
    //  with border and/or status bar.
    viewwindowx = (native_render_width-width) >> 1; 

    // Column offset. For windows.
    for (i=0 ; i<width ; i++)
	columnofs[i] = viewwindowx + i;

    // Samw with base row offset.
    if (width == native_render_width) 
	viewwindowy = 0; 
    else 
	viewwindowy = (native_render_height-SBARHEIGHT-height) >> 1; 

    // Preclaculate all row offsets.
    for (i=0 ; i<height ; i++) 
	ylookup[i] = Native_RenderPixels() + (i+viewwindowy)*native_render_width; 
} 
 
 


//
// R_FillBackScreen
// Fills the back screen with a pattern
//  for variable screen sizes
// Also draws a beveled edge.
//
void R_FillBackScreen (void) 
{ 
    byte*	src;
    byte*	dest; 
    int		x;
    int		y; 
    patch_t*	patch;

    // DOOM border patch.
    char	name1[] = "FLOOR7_2";

    // DOOM II border patch.
    char	name2[] = "GRNROCK";	

    char*	name;
	
    if (scaledviewwidth == native_render_width)
	return;
	
    if ( gamemode == commercial)
	name = name2;
    else
	name = name1;
    
    src = W_CacheLumpName (name, PU_CACHE); 
    dest = screens[1]; 
	 
    for (y=0 ; y<native_render_height-SBARHEIGHT ; y++) 
    { 
	for (x=0 ; x<native_render_width/64 ; x++) 
	{ 
	    memcpy (dest, src+((y&63)<<6), 64); 
	    dest += 64; 
	} 

	if (native_render_width&63) 
	{ 
	    memcpy (dest, src+((y&63)<<6), native_render_width&63); 
	    dest += (native_render_width&63); 
	} 
    } 
	
    patch = W_CacheLumpName ("brdr_t",PU_CACHE);

    for (x=0 ; x<scaledviewwidth ; x+=8)
	V_DrawPatch (viewwindowx+x,viewwindowy-8,1,patch);
    patch = W_CacheLumpName ("brdr_b",PU_CACHE);

    for (x=0 ; x<scaledviewwidth ; x+=8)
	V_DrawPatch (viewwindowx+x,viewwindowy+viewheight,1,patch);
    patch = W_CacheLumpName ("brdr_l",PU_CACHE);

    for (y=0 ; y<viewheight ; y+=8)
	V_DrawPatch (viewwindowx-8,viewwindowy+y,1,patch);
    patch = W_CacheLumpName ("brdr_r",PU_CACHE);

    for (y=0 ; y<viewheight ; y+=8)
	V_DrawPatch (viewwindowx+scaledviewwidth,viewwindowy+y,1,patch);


    // Draw beveled edge. 
    V_DrawPatch (viewwindowx-8,
		 viewwindowy-8,
		 1,
		 W_CacheLumpName ("brdr_tl",PU_CACHE));
    
    V_DrawPatch (viewwindowx+scaledviewwidth,
		 viewwindowy-8,
		 1,
		 W_CacheLumpName ("brdr_tr",PU_CACHE));
    
    V_DrawPatch (viewwindowx-8,
		 viewwindowy+viewheight,
		 1,
		 W_CacheLumpName ("brdr_bl",PU_CACHE));
    
    V_DrawPatch (viewwindowx+scaledviewwidth,
		 viewwindowy+viewheight,
		 1,
		 W_CacheLumpName ("brdr_br",PU_CACHE));
} 
 

//
// Copy a screen buffer.
//
void
R_VideoErase
( unsigned	ofs,
  int		count ) 
{ 
  // LFB copy.
  // This might not be a good idea if memcpy
  //  is not optiomal, e.g. byte by byte on
  //  a 32bit CPU, as GNU GCC/Linux libc did
  //  at one point.
    memcpy (screens[0]+ofs, screens[1]+ofs, count); 
} 


//
// R_DrawViewBorder
// Draws the border around the view
//  for different size windows?
//
void
V_MarkRect
( int		x,
  int		y,
  int		width,
  int		height ); 
 
void R_DrawViewBorder (void) 
{ 
    int		top;
    int		side;
    int		ofs;
    int		i; 
 
    if (scaledviewwidth == native_render_width) 
	return; 
  
    top = ((native_render_height-SBARHEIGHT)-viewheight)/2; 
    side = (native_render_width-scaledviewwidth)/2; 
 
    // copy top and one line of left side 
    R_VideoErase (0, top*native_render_width+side); 
 
    // copy one line of right side and bottom 
    ofs = (viewheight+top)*native_render_width-side; 
    R_VideoErase (ofs, top*native_render_width+side); 
 
    // copy sides using wraparound 
    ofs = top*native_render_width + native_render_width-side; 
    side <<= 1;
    
    for (i=1 ; i<viewheight ; i++) 
    { 
	R_VideoErase (ofs, side); 
	ofs += native_render_width; 
    } 

    // ? 
    V_MarkRect (0,0,native_render_width, native_render_height-SBARHEIGHT); 
} 
 
 
