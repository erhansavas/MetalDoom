// SPDX-License-Identifier: GPL-2.0-only
#include "Native.h"
extern "C" {
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "i_net.h"
#include "m_misc.h"
#include "g_game.h"
#include "v_video.h"
}
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" {
void I_Init(void) { I_InitSound(); I_InitMusic(); }
byte *I_ZoneBase(int *size) { *size=32*1024*1024; auto *p=(byte *)calloc(1,*size); if(!p) I_Error((char *)"Out of memory"); return p; }
byte *I_AllocLow(int size) { auto *p=(byte *)calloc(1,size); if(!p) I_Error((char *)"Out of memory"); return p; }
int I_GetTime(void) { return Native_Time(); }
void I_StartFrame(void) { Native_Pump(); Native_BeginFrame(); Native_AudioService(); Native_FeedbackUpdate(); }
void I_StartTic(void) { Native_Pump(); Native_ControllerPoll(); }
void I_WaitVBL(int count) { usleep(count*(1000000/70)); }
void I_Tactile(int,int,int) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_InitGraphics(void) {}
void I_ShutdownGraphics(void) { Native_Shutdown(); }
void I_UpdateNoBlit(void) { Native_Pump(); }
void I_FinishUpdate(void) { Native_Present(screens[0]); }
void I_ReadScreen(byte *screen) { memcpy(screen,screens[0],320*200); }
void I_SetPalette(byte *palette) { Native_SetPalette(palette); }
void I_InitNetwork(void) {
    doomcom=(doomcom_t *)calloc(1,sizeof(*doomcom));
    if(!doomcom) I_Error((char *)"Out of memory");
    doomcom->id=DOOMCOM_ID; doomcom->ticdup=1;
    doomcom->numnodes=1; doomcom->numplayers=1;
    doomcom->consoleplayer=0; netgame=false;
}
void I_NetCmd(void) { I_Error((char *)"Network multiplayer is not implemented"); }
int native_exitstatus=0;
void I_Quit(void) {
    if(demorecording) G_CheckDemoStatus();
    D_QuitNetGame(); M_SaveDefaults();
    I_ShutdownSound(); I_ShutdownMusic(); Native_Shutdown(); exit(native_exitstatus);
}
void I_Error(char *format,...) {
    char text[2048]; va_list ap; va_start(ap,format); vsnprintf(text,sizeof(text),format,ap); va_end(ap);
    fprintf(stderr,"DOOM: %s\n",text);
    Native_ControllerReset(); I_ShutdownSound(); I_ShutdownMusic(); Native_Error(text); exit(1);
}
}
