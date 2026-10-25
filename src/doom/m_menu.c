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
//	DOOM selection menu, options, episode etc.
//	Sliders and icons. Kinda widget stuff.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_menu.c,v 1.7 1997/02/03 22:45:10 b1 Exp $";

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <ctype.h>


#include "doomdef.h"
#include "Native.h"
#include "dstrings.h"

#include "d_main.h"

#include "i_system.h"
#include "i_video.h"
#include "z_zone.h"
#include "v_video.h"
#include "w_wad.h"

#include "r_local.h"


#include "hu_stuff.h"

#include "g_game.h"

#include "m_argv.h"
#include "m_swap.h"

#include "s_sound.h"

#include "doomstat.h"

// Data.
#include "sounds.h"

#include "m_menu.h"
#include "am_map.h"
#include "p_tick.h"

void P_DamageMobj (mobj_t* target, mobj_t* inflictor, mobj_t* source, int damage);
void P_MobjThinker (mobj_t* mobj);
extern thinker_t thinkercap;



extern patch_t*		hu_font[HU_FONTSIZE];
extern boolean		message_dontfuckwithme;

extern boolean		chat_on;		// in heads-up code

//
// defaulted values
//
int			mouseSensitivity;       // has default

// Show messages has default, 0 = off, 1 = on
int			showMessages;
	

// Blocky mode, has default, 0 = high, 1 = normal
int			detailLevel;		
int			screenblocks;		// has default

// temp for screenblocks (0-9)
int			screenSize;		

// -1 = no quicksave slot picked!
int			quickSaveSlot;          

 // 1 = message to be printed
int			messageToPrint;
// ...and here is the message string!
char*			messageString;		

// message x & y
int			messx;			
int			messy;
int			messageLastMenuActive;

// timed message = no input from user
boolean			messageNeedsInput;
// Answer highlighted on the native panel: 0 yes, 1 no.
static int		messageChoice;
// Choices that drop the player into play (new game, load, resume) answer
// with the shotgun being racked instead of the ordinary menu shot.
static int menuEnterSound = sfx_pistol;
static void M_EnterPlaySound(void) { menuEnterSound = sfx_sgcock; }

void    (*messageRoutine)(int response);

#define SAVESTRINGSIZE 	24

char gammamsg[5][26] =
{
    GAMMALVL0,
    GAMMALVL1,
    GAMMALVL2,
    GAMMALVL3,
    GAMMALVL4
};

// we are going to be entering a savegame string
int			saveStringEnter;              
int             	saveSlot;	// which slot to save in
int			saveCharIndex;	// which char we're editing
// old save description before edit
char			saveOldString[SAVESTRINGSIZE];  

boolean			inhelpscreens;
boolean			menuactive;

#define SKULLXOFF		-32
#define LINEHEIGHT		16

extern boolean		sendpause;
char			savegamestrings[10][SAVESTRINGSIZE];
// The suggested name is still untouched: the first typed key replaces it.
static boolean		saveNameFresh;

char	endstring[160];


//
// MENU TYPEDEFS
//
typedef struct
{
    // 0 = no cursor here, 1 = ok, 2 = arrows ok
    short	status;
    
    char	name[10];
    
    // choice = menu item #.
    // if status = 2,
    //   choice=0:leftarrow,1:rightarrow
    void	(*routine)(int choice);
    
    // hotkey in menu
    char	alphaKey;			
} menuitem_t;



typedef struct menu_s
{
    short		numitems;	// # of menu items
    struct menu_s*	prevMenu;	// previous menu
    menuitem_t*		menuitems;	// menu items
    void		(*routine)();	// draw routine
    short		x;
    short		y;		// x,y of menu
    short		lastOn;		// last item user was on in menu
} menu_t;

short		itemOn;			// menu item skull is on
short		skullAnimCounter;	// skull animation counter
short		whichSkull;		// which skull to draw

// graphic name of skulls
// warning: initializer-string for array of chars is too long
char    skullName[2][/*8*/9] = {"M_SKULL1","M_SKULL2"};

// current menudef
menu_t*	currentMenu;                          

//
// PROTOTYPES
//
void M_NewGame(int choice);
void M_Episode(int choice);
void M_ChooseSkill(int choice);
void M_LoadGame(int choice);
void M_SaveGame(int choice);
void M_Options(int choice);
void M_EndGame(int choice);
void M_ReadThis(int choice);
void M_ReadThis2(int choice);
void M_QuitDOOM(int choice);

void M_ChangeMessages(int choice);
void M_ChangeSensitivity(int choice);
void M_MusicVol(int choice);
void M_ChangeDetail(int choice);
void M_SizeDisplay(int choice);
void M_StartGame(int choice);
void M_Sound(int choice);

void M_FinishReadThis(int choice);
void M_LoadSelect(int choice);
void M_SaveSelect(int choice);
void M_ReadSaveStrings(void);
void M_QuickSave(void);
void M_QuickLoad(void);

void M_DrawMainMenu(void);
void M_DrawReadThis1(void);
void M_DrawReadThis2(void);
void M_DrawNewGame(void);
void M_DrawEpisode(void);
void M_DrawOptions(void);
void M_DrawSound(void);
void M_DrawLoad(void);
void M_DrawSave(void);

void M_DrawSaveLoadBorder(int x,int y);
void M_SetupNextMenu(menu_t *menudef);
void M_DrawThermo(int x,int y,int thermWidth,int thermDot);
void M_DrawEmptyCell(menu_t *menu,int item);
void M_DrawSelCell(menu_t *menu,int item);
void M_WriteText(int x, int y, char *string);
int  M_StringWidth(char *string);
int  M_StringHeight(char *string);
void M_StartControlPanel(void);
void M_StartMessage(char *string,void *routine,boolean input);
void M_StopMessage(void);
void M_ClearMenus (void);




//
// DOOM MENU
//
enum
{
    resume = 0,
    quicksave,
    savegame,
    loadgame,
    newgame,
    options,
    endgamemain,
    readthis,
    quitdoom,
    main_end
} main_e;

void M_QuickSave(void);
static void M_NativeQuickSave(int choice) { M_QuickSave(); }
static void M_Resume(int choice);

menuitem_t MainMenu[]=
{
    // Pause menu priority: back to play, saving, then the rest.
    {-1,"",M_Resume,0},
    {-1,"",M_NativeQuickSave,'k'},
    {1,"M_SAVEG",M_SaveGame,'s'},
    {1,"M_LOADG",M_LoadGame,'l'},
    {1,"M_NGAME",M_NewGame,'n'},
    {1,"M_OPTION",M_Options,'o'},
    {-1,"",M_EndGame,'e'},
    // Another hickup with Special edition.
    {1,"M_RDTHIS",M_ReadThis,'r'},
    {1,"M_QUITG",M_QuitDOOM,'q'}
};

menu_t  MainDef =
{
    main_end,
    NULL,
    MainMenu,
    M_DrawMainMenu,
    97,64,
    0
};


//
// EPISODE SELECT
//
enum
{
    ep1,
    ep2,
    ep3,
    ep4,
    ep_end
} episodes_e;

menuitem_t EpisodeMenu[]=
{
    {1,"M_EPI1", M_Episode,'k'},
    {1,"M_EPI2", M_Episode,'t'},
    {1,"M_EPI3", M_Episode,'i'},
    {1,"M_EPI4", M_Episode,'t'}
};

menu_t  EpiDef =
{
    ep_end,		// # of menu items
    &MainDef,		// previous menu
    EpisodeMenu,	// menuitem_t ->
    M_DrawEpisode,	// drawing routine ->
    48,63,              // x,y
    ep1			// lastOn
};

//
// NEW GAME
//
enum
{
    killthings,
    toorough,
    hurtme,
    violence,
    nightmare,
    newg_end
} newgame_e;

menuitem_t NewGameMenu[]=
{
    {1,"M_JKILL",	M_ChooseSkill, 'i'},
    {1,"M_ROUGH",	M_ChooseSkill, 'h'},
    {1,"M_HURT",	M_ChooseSkill, 'h'},
    {1,"M_ULTRA",	M_ChooseSkill, 'u'},
    {1,"M_NMARE",	M_ChooseSkill, 'n'}
};

menu_t  NewDef =
{
    newg_end,		// # of menu items
    &EpiDef,		// previous menu
    NewGameMenu,	// menuitem_t ->
    M_DrawNewGame,	// drawing routine ->
    48,63,              // x,y
    hurtme		// lastOn
};



//
// OPTIONS MENU
//
enum
{
    endgame,
    messages,
    detail,
    scrnsize,
    option_empty1,
    mousesens,
    option_empty2,
    soundvol,
    videoopt,
    controlsopt,
    controlleropt,
    opt_end
} options_e;

// Native settings (2026-10-10): the settings grouped by category. The
// original Options rows stay in place for the classic presentation.
static void M_NativePage(menu_t *page);
static void M_Video(int choice);
static void M_ControlsPage(int choice);
static void M_ControllerPage(int choice);
static void M_ToggleStatusBar(int choice) { Native_SetStatusBar(!Native_StatusBar()); S_StartSound(NULL,sfx_pistol); }
static void M_ToggleFullscreen(int choice) { Native_ToggleFullscreen(); S_StartSound(NULL,sfx_pistol); }
static void M_ToggleSkipTransitions(int choice) { Native_SetSkipTransitions(!Native_SkipTransitions()); S_StartSound(NULL,sfx_pistol); }

menuitem_t OptionsMenu[]=
{
    {1,"M_ENDGAM",	M_EndGame,'e'},
    {1,"M_MESSG",	M_ChangeMessages,'m'},
    {1,"M_DETAIL",	M_ChangeDetail,'g'},
    {2,"M_SCRNSZ",	M_SizeDisplay,'s'},
    {-1,"",0},
    {2,"M_MSENS",	M_ChangeSensitivity,'m'},
    {-1,"",0},
    {1,"M_SVOL",	M_Sound,'a'},
    {1,"",	M_Video,'v'},
    {1,"",	M_ControlsPage,'c'},
    {1,"",	M_ControllerPage,'p'}
};

menu_t  OptionsDef =
{
    opt_end,
    &MainDef,
    OptionsMenu,
    M_DrawOptions,
    60,37,
    0
};

enum { video_fullscreen, video_hud, video_messages, video_skip, video_end };
menuitem_t VideoMenu[]=
{
    {1,"",M_ToggleFullscreen,'f'},
    {1,"",M_ToggleStatusBar,'s'},
    {1,"",M_ChangeMessages,'m'},
    {1,"",M_ToggleSkipTransitions,'t'}
};
menu_t VideoDef = { video_end, &OptionsDef, VideoMenu, NULL, 60, 37, 0 };

static void M_PadSetting(int setting, int choice) { Native_PadAdjust(setting, choice?1:-1); S_StartSound(NULL,sfx_stnmov); }
static void M_PadHaptics(int choice) { M_PadSetting(NATIVE_PAD_HAPTICS, choice); }
static void M_PadHapticStrength(int choice) { M_PadSetting(NATIVE_PAD_HAPTIC_STRENGTH, choice); }
static void M_PadTriggers(int choice) { M_PadSetting(NATIVE_PAD_TRIGGERS, choice); }
static void M_PadTriggerStrength(int choice) { M_PadSetting(NATIVE_PAD_TRIGGER_STRENGTH, choice); }
static void M_PadDeadzone(int choice) { M_PadSetting(NATIVE_PAD_DEADZONE, choice); }
static void M_PadTurn(int choice) { M_PadSetting(NATIVE_PAD_TURN, choice); }

// Controls: the mouse, and the controls reference that was the main menu's
// "Read This!".
enum { ctl_mouse, ctl_help, ctl_end };
menuitem_t ControlsMenu[]=
{
    {2,"",M_ChangeSensitivity,'m'},
    {1,"",M_ReadThis,'s'}
};
menu_t ControlsDef = { ctl_end, &OptionsDef, ControlsMenu, NULL, 60, 37, 0 };

enum { pad_haptics, pad_hapticstrength, pad_triggers, pad_triggerstrength, pad_deadzone, pad_turn, pad_end };
menuitem_t ControllerMenu[]=
{
    {2,"",M_PadHaptics,'h'},
    {2,"",M_PadHapticStrength,'v'},
    {2,"",M_PadTriggers,'t'},
    {2,"",M_PadTriggerStrength,'r'},
    {2,"",M_PadDeadzone,'d'},
    {2,"",M_PadTurn,'s'}
};
menu_t ControllerDef = { pad_end, &OptionsDef, ControllerMenu, NULL, 60, 37, 0 };

static void M_NativePage(menu_t *page)
{
    page->lastOn=0;
    M_SetupNextMenu(page);
}
static void M_Video(int choice) { M_NativePage(&VideoDef); }
static void M_ControlsPage(int choice) { M_NativePage(&ControlsDef); }
static void M_ControllerPage(int choice) { M_NativePage(&ControllerDef); }
//
// Read This! MENU 1 & 2
//
enum
{
    rdthsempty1,
    read1_end
} read_e;

menuitem_t ReadMenu1[] =
{
    {1,"",M_ReadThis2,0}
};

menu_t  ReadDef1 =
{
    read1_end,
    &MainDef,
    ReadMenu1,
    M_DrawReadThis1,
    280,185,
    0
};

enum
{
    rdthsempty2,
    read2_end
} read_e2;

menuitem_t ReadMenu2[]=
{
    {1,"",M_FinishReadThis,0}
};

menu_t  ReadDef2 =
{
    read2_end,
    &ReadDef1,
    ReadMenu2,
    M_DrawReadThis2,
    330,175,
    0
};

//
// SOUND VOLUME MENU
//
enum
{
    music_vol,
    sound_end
} sound_e;

menuitem_t SoundMenu[]=
{
    {1,"M_MUSVOL",M_MusicVol,'m'}
};

menu_t  SoundDef =
{
    sound_end,
    &OptionsDef,
    SoundMenu,
    M_DrawSound,
    80,64,
    0
};

//
// LOAD GAME MENU
//
enum
{
    load1,
    load2,
    load3,
    load4,
    load5,
    load6,
    load_end
} load_e;

menuitem_t LoadMenu[]=
{
    {1,"", M_LoadSelect,'1'},
    {1,"", M_LoadSelect,'2'},
    {1,"", M_LoadSelect,'3'},
    {1,"", M_LoadSelect,'4'},
    {1,"", M_LoadSelect,'5'},
    {1,"", M_LoadSelect,'6'}
};

menu_t  LoadDef =
{
    load_end,
    &MainDef,
    LoadMenu,
    M_DrawLoad,
    80,54,
    0
};

//
// SAVE GAME MENU
//
menuitem_t SaveMenu[]=
{
    {1,"", M_SaveSelect,'1'},
    {1,"", M_SaveSelect,'2'},
    {1,"", M_SaveSelect,'3'},
    {1,"", M_SaveSelect,'4'},
    {1,"", M_SaveSelect,'5'},
    {1,"", M_SaveSelect,'6'}
};

menu_t  SaveDef =
{
    load_end,
    &MainDef,
    SaveMenu,
    M_DrawSave,
    80,54,
    0
};


//
// M_ReadSaveStrings
//  read the strings from the savegame files
//
// A save names itself after where it was made, so the name is never required.
static void M_AutoSaveName(char *name,int size)
{
    int seconds = leveltime/TICRATE;
    if (gamemode == commercial)
	snprintf(name,size,"MAP%02d  %d:%02d",gamemap,seconds/60,seconds%60);
    else
	snprintf(name,size,"E%dM%d  %d:%02d",gameepisode,gamemap,seconds/60,seconds%60);
}

void M_ReadSaveStrings(void)
{
    int             handle;
    int             count;
    int             i;
    char    name[4096];
	
    for (i = 0;i < load_end;i++)
    {
        snprintf(name,sizeof(name),"%s",Native_SavePath(i));
	handle = open (name, O_RDONLY | 0, 0666);
	if (handle == -1 || !Native_SaveExists(i))
	{
	    if (handle != -1)
		close (handle);
	    strcpy(&savegamestrings[i][0],EMPTYSTRING);
	    LoadMenu[i].status = 0;
	    continue;
	}
	count = read (handle, &savegamestrings[i], SAVESTRINGSIZE);
	close (handle);
        savegamestrings[i][SAVESTRINGSIZE-1]=0;
	LoadMenu[i].status = count==SAVESTRINGSIZE;
    }
}


//
// M_LoadGame & Cie.
//
void M_DrawLoad(void)
{
    int             i;
	
    V_DrawPatchDirect (72,28,0,W_CacheLumpName("M_LOADG",PU_CACHE));
    for (i = 0;i < load_end; i++)
    {
	M_DrawSaveLoadBorder(LoadDef.x,LoadDef.y+LINEHEIGHT*i);
	M_WriteText(LoadDef.x,LoadDef.y+LINEHEIGHT*i,savegamestrings[i]);
    }
}



//
// Draw border for the savegame description
//
void M_DrawSaveLoadBorder(int x,int y)
{
    int             i;
	
    V_DrawPatchDirect (x-8,y+7,0,W_CacheLumpName("M_LSLEFT",PU_CACHE));
	
    for (i = 0;i < 24;i++)
    {
	V_DrawPatchDirect (x,y+7,0,W_CacheLumpName("M_LSCNTR",PU_CACHE));
	x += 8;
    }

    V_DrawPatchDirect (x,y+7,0,W_CacheLumpName("M_LSRGHT",PU_CACHE));
}



//
// User wants to load this game
//
void M_LoadSelect(int choice)
{
    char    name[4096];
	
    snprintf(name,sizeof(name),"%s",Native_SavePath(choice));
    G_LoadGame (name);
    M_ClearMenus ();
    M_EnterPlaySound ();
}

//
// Selected from DOOM menu
//
void M_LoadGame (int choice)
{
    if (netgame)
    {
	M_StartMessage(LOADNET,NULL,false);
	return;
    }
	
    M_SetupNextMenu(&LoadDef);
    M_ReadSaveStrings();
}


//
//  M_SaveGame & Cie.
//
void M_DrawSave(void)
{
    int             i;
	
    V_DrawPatchDirect (72,28,0,W_CacheLumpName("M_SAVEG",PU_CACHE));
    for (i = 0;i < load_end; i++)
    {
	M_DrawSaveLoadBorder(LoadDef.x,LoadDef.y+LINEHEIGHT*i);
	M_WriteText(LoadDef.x,LoadDef.y+LINEHEIGHT*i,savegamestrings[i]);
    }
	
    if (saveStringEnter)
    {
	i = M_StringWidth(savegamestrings[saveSlot]);
	M_WriteText(LoadDef.x + i,LoadDef.y+LINEHEIGHT*saveSlot,"_");
    }
}

//
// M_Responder calls this when user is finished
//
void M_DoSave(int slot)
{
    if (!savegamestrings[slot][0])
	M_AutoSaveName(savegamestrings[slot],SAVESTRINGSIZE);
    G_SaveGame (slot,savegamestrings[slot]);
    M_ClearMenus ();

    // PICK QUICKSAVE SLOT YET?
    if (quickSaveSlot == -2)
	quickSaveSlot = slot;
}

//
// User wants to save. Start string input for M_Responder
//
void M_SaveSelect(int choice)
{
    // we are going to be intercepting all chars
    saveStringEnter = 1;
    
    saveSlot = choice;
    strcpy(saveOldString,savegamestrings[choice]);
    M_AutoSaveName(savegamestrings[choice],SAVESTRINGSIZE);
    saveNameFresh = true;
    saveCharIndex = strlen(savegamestrings[choice]);
}

//
// Selected from DOOM menu
//
void M_SaveGame (int choice)
{
    if (!usergame || players[consoleplayer].health<=0)
    {
	M_StartMessage(SAVEDEAD,NULL,false);
	return;
    }
	
    if (gamestate != GS_LEVEL)
	return;
	
    M_SetupNextMenu(&SaveDef);
    M_ReadSaveStrings();
}



//
//      M_QuickSave
//
char    tempstring[80];

void M_QuickSaveResponse(int ch)
{
    if (ch == 'y')
    {
	M_DoSave(quickSaveSlot);
	S_StartSound(NULL,sfx_swtchx);
    }
}

void M_QuickSave(void)
{
    if (!usergame || players[consoleplayer].health<=0)
    {
	S_StartSound(NULL,sfx_oof);
	return;
    }

    if (gamestate != GS_LEVEL)
	return;
	
    // The first quick save adopts the newest save.
    if(quickSaveSlot<0) quickSaveSlot=Native_LatestSave();
    if (quickSaveSlot < 0)
    {
	M_StartControlPanel();
	M_ReadSaveStrings();
	M_SetupNextMenu(&SaveDef);
	quickSaveSlot = -2;	// means to pick a slot now
	return;
    }
    M_ReadSaveStrings();
    snprintf(tempstring,sizeof(tempstring),QSPROMPT,savegamestrings[quickSaveSlot]);
    M_StartMessage(tempstring,M_QuickSaveResponse,true);
}



//
// M_QuickLoad
//
static int quickLoadSlot = -1;
void M_QuickLoadResponse(int ch)
{
    if (ch == 'y')
    {
	M_LoadSelect(quickLoadSlot);
	S_StartSound(NULL,sfx_swtchx);
    }
}


void M_QuickLoad(void)
{
    if (netgame)
    {
	M_StartMessage(QLOADNET,NULL,false);
	return;
    }
	
    // Quick load resumes the quick-save slot or else the newest save.
    quickLoadSlot = quickSaveSlot>=0 ? quickSaveSlot : Native_LatestSave();
    if (quickLoadSlot < 0)
    {
	M_StartMessage(QSAVESPOT,NULL,false);
	return;
    }
    M_ReadSaveStrings();
    snprintf(tempstring,sizeof(tempstring),QLPROMPT,savegamestrings[quickLoadSlot]);
    M_StartMessage(tempstring,M_QuickLoadResponse,true);
}




//
// Read This Menus
// Had a "quick hack to fix romero bug"
//
void M_DrawReadThis1(void)
{
    inhelpscreens = true;
    switch ( gamemode )
    {
      case commercial:
	V_DrawPatchDirect (0,0,0,W_CacheLumpName("HELP",PU_CACHE));
	break;
      case shareware:
      case registered:
      case retail:
	V_DrawPatchDirect (0,0,0,W_CacheLumpName("HELP1",PU_CACHE));
	break;
      default:
	break;
    }
    return;
}



//
// Read This Menus - optional second page.
//
void M_DrawReadThis2(void)
{
    inhelpscreens = true;
    switch ( gamemode )
    {
      case retail:
      case commercial:
	// This hack keeps us from having to change menus.
	V_DrawPatchDirect (0,0,0,W_CacheLumpName("CREDIT",PU_CACHE));
	break;
      case shareware:
      case registered:
	V_DrawPatchDirect (0,0,0,W_CacheLumpName("HELP2",PU_CACHE));
	break;
      default:
	break;
    }
    return;
}


//
// Change Sfx & Music volumes
//
void M_DrawSound(void)
{
    V_DrawPatchDirect (60,38,0,W_CacheLumpName("M_SVOL",PU_CACHE));

}

void M_Sound(int choice)
{
    M_SetupNextMenu(&SoundDef);
}


// Native port (2026-10-10): Volume is fixed (see M_LoadDefaults); music is a switch and
// follows Volume at a fixed balance (see S_ApplyMusicBalance).
void M_MusicVol(int choice)
{
    Native_SetMusicEnabled(!Native_MusicEnabled());
    S_ApplyMusicBalance();
}




//
// M_DrawMainMenu
//
void M_DrawMainMenu(void)
{
    V_DrawPatchDirect (94,2,0,W_CacheLumpName("M_DOOM",PU_CACHE));
}




//
// M_NewGame
//
void M_DrawNewGame(void)
{
    V_DrawPatchDirect (96,14,0,W_CacheLumpName("M_NEWG",PU_CACHE));
    V_DrawPatchDirect (54,38,0,W_CacheLumpName("M_SKILL",PU_CACHE));
}

void M_NewGame(int choice)
{
    if (netgame && !demoplayback)
    {
	M_StartMessage(NEWGAME,NULL,false);
	return;
    }
	
    if ( gamemode == commercial )
	M_SetupNextMenu(&NewDef);
    else
	M_SetupNextMenu(&EpiDef);
}


//
//      M_Episode
//
int     epi;

void M_DrawEpisode(void)
{
    V_DrawPatchDirect (54,38,0,W_CacheLumpName("M_EPISOD",PU_CACHE));
}

void M_VerifyNightmare(int ch)
{
    if (ch != 'y')
	return;
		
    G_DeferedInitNew(nightmare,epi+1,1);
    M_ClearMenus ();
    M_EnterPlaySound ();
}

static void M_Resume(int choice)
{
    (void)choice;
    M_ClearMenus ();
    // Resume also ends a pause (a controller that disconnected pauses play).
    if (paused)
	sendpause = true;
    M_EnterPlaySound ();
}

void M_ChooseSkill(int choice)
{
    if (choice == nightmare)
    {
	M_StartMessage(NIGHTMARE,M_VerifyNightmare,true);
	return;
    }
	
    G_DeferedInitNew(choice,epi+1,1);
    M_ClearMenus ();
    M_EnterPlaySound ();
}

void M_Episode(int choice)
{
    if ( (gamemode == shareware)
	 && choice)
    {
	M_StartMessage(SWSTRING,NULL,false);
	M_SetupNextMenu(&ReadDef1);
	return;
    }

    // Yet another hack...
    if ( (gamemode == registered)
	 && (choice > 2))
    {
      fprintf( stderr,
	       "M_Episode: 4th episode requires UltimateDOOM\n");
      choice = 0;
    }
	 
    epi = choice;
    M_SetupNextMenu(&NewDef);
}



//
// M_Options
//
char    detailNames[2][9]	= {"M_GDHIGH","M_GDLOW"};
char	msgNames[2][9]		= {"M_MSGOFF","M_MSGON"};


void M_DrawOptions(void)
{
    V_DrawPatchDirect (108,15,0,W_CacheLumpName("M_OPTTTL",PU_CACHE));
	
    V_DrawPatchDirect (OptionsDef.x + 175,OptionsDef.y+LINEHEIGHT*detail,0,
		       W_CacheLumpName(detailNames[detailLevel],PU_CACHE));

    V_DrawPatchDirect (OptionsDef.x + 120,OptionsDef.y+LINEHEIGHT*messages,0,
		       W_CacheLumpName(msgNames[showMessages],PU_CACHE));

    M_DrawThermo(OptionsDef.x,OptionsDef.y+LINEHEIGHT*(mousesens+1),
		 10,mouseSensitivity);
	
    M_DrawThermo(OptionsDef.x,OptionsDef.y+LINEHEIGHT*(scrnsize+1),
		 9,screenSize);
}

void M_Options(int choice)
{
    M_SetupNextMenu(&OptionsDef);
}



//
//      Toggle messages on/off
//
void M_ChangeMessages(int choice)
{
    // warning: unused parameter `int choice'
    (void)choice;
    showMessages = 1 - showMessages;
	
    if (!showMessages)
	players[consoleplayer].message = MSGOFF;
    else
	players[consoleplayer].message = MSGON ;

    message_dontfuckwithme = true;
}


//
// M_EndGame
//
void M_EndGameResponse(int ch)
{
    if (ch != 'y')
	return;
		
    currentMenu->lastOn = itemOn;
    M_ClearMenus ();
    D_StartTitle ();
}

void M_EndGame(int choice)
{
    (void)choice;
    if (!usergame || players[consoleplayer].health<=0)
    {
	S_StartSound(NULL,sfx_oof);
	return;
    }
	
    if (netgame)
    {
	M_StartMessage(NETEND,NULL,false);
	return;
    }
	
    M_StartMessage(ENDGAME,M_EndGameResponse,true);
}




//
// M_ReadThis
//
void M_ReadThis(int choice)
{
    (void)choice;
    M_SetupNextMenu(&ReadDef1);
}

void M_ReadThis2(int choice)
{
    (void)choice;
    M_SetupNextMenu(&ReadDef2);
}

void M_FinishReadThis(int choice)
{
    (void)choice;
    M_SetupNextMenu(Native_Enhanced() ? &ControlsDef : &MainDef);
}




//
// M_QuitDOOM
//
int     quitsounds[8] =
{
    sfx_pldeth,
    sfx_dmpain,
    sfx_popain,
    sfx_slop,
    sfx_telept,
    sfx_posit1,
    sfx_posit3,
    sfx_sgtatk
};

int     quitsounds2[8] =
{
    sfx_vilact,
    sfx_getpow,
    sfx_boscub,
    sfx_slop,
    sfx_skeswg,
    sfx_kntdth,
    sfx_bspact,
    sfx_sgtatk
};



void M_QuitResponse(int ch)
{
    if (ch != 'y')
	return;
    if (!netgame)
    {
	if (gamemode == commercial)
	    S_StartSound(NULL,quitsounds2[(gametic>>2)&7]);
	else
	    S_StartSound(NULL,quitsounds[(gametic>>2)&7]);
    }
    // The quit sound plays out while the window fades away.
    Native_QuitFade ();
    I_Quit ();
}




// Command-Q: the quit confirmation over whatever is on screen.
// Command-Q: the quit confirmation, or straight out when another question
// is already on screen.
void M_NativeQuitPrompt(void)
{
  if (messageToPrint)
  {
    if (messageRoutine != M_QuitResponse)
      I_Quit();
    return;
  }
  S_StartSound(NULL,sfx_swtchn);
  M_QuitDOOM(0);
}

void M_QuitDOOM(int choice)
{
  // We pick index 0 which is language sensitive,
  //  or one at random, between 1 and maximum number.
  if (language != english )
    sprintf(endstring,"%s\n\n"DOSY, endmsg[0] );
  else
    sprintf(endstring,"%s\n\n"DOSY, endmsg[ (gametic%(NUM_QUITMESSAGES-2))+1 ]);
  
  M_StartMessage(endstring,M_QuitResponse,true);
}




void M_ChangeSensitivity(int choice)
{
    switch(choice)
    {
      case 0:
	if (mouseSensitivity)
	    mouseSensitivity--;
	break;
      case 1:
	if (mouseSensitivity < 9)
	    mouseSensitivity++;
	break;
    }
}




void M_ChangeDetail(int choice)
{
    (void)choice;
    detailLevel = 1 - detailLevel;

    // FIXME - does not work. Remove anyway?
    fprintf( stderr, "M_ChangeDetail: low detail mode n.a.\n");

    return;
    
    /*R_SetViewSize (screenblocks, detailLevel);

    if (!detailLevel)
	players[consoleplayer].message = DETAILHI;
    else
	players[consoleplayer].message = DETAILLO;*/
}




void M_SizeDisplay(int choice)
{
    switch(choice)
    {
      case 0:
	if (screenSize > 0)
	{
	    screenblocks--;
	    screenSize--;
	}
	break;
      case 1:
	if (screenSize < 8)
	{
	    screenblocks++;
	    screenSize++;
	}
	break;
    }
	

    R_SetViewSize (screenblocks, detailLevel);
}




//
//      Menu Functions
//
void
M_DrawThermo
( int	x,
  int	y,
  int	thermWidth,
  int	thermDot )
{
    int		xx;
    int		i;

    xx = x;
    V_DrawPatchDirect (xx,y,0,W_CacheLumpName("M_THERML",PU_CACHE));
    xx += 8;
    for (i=0;i<thermWidth;i++)
    {
	V_DrawPatchDirect (xx,y,0,W_CacheLumpName("M_THERMM",PU_CACHE));
	xx += 8;
    }
    V_DrawPatchDirect (xx,y,0,W_CacheLumpName("M_THERMR",PU_CACHE));

    V_DrawPatchDirect ((x+8) + thermDot*8,y,
		       0,W_CacheLumpName("M_THERMO",PU_CACHE));
}



void
M_DrawEmptyCell
( menu_t*	menu,
  int		item )
{
    V_DrawPatchDirect (menu->x - 10,        menu->y+item*LINEHEIGHT - 1, 0,
		       W_CacheLumpName("M_CELL1",PU_CACHE));
}

void
M_DrawSelCell
( menu_t*	menu,
  int		item )
{
    V_DrawPatchDirect (menu->x - 10,        menu->y+item*LINEHEIGHT - 1, 0,
		       W_CacheLumpName("M_CELL2",PU_CACHE));
}


void
M_StartMessage
( char*		string,
  void*		routine,
  boolean	input )
{
    messageLastMenuActive = menuactive;
    messageToPrint = 1;
    messageString = string;
    messageRoutine = routine;
    messageNeedsInput = input;
    messageChoice = 0;
    menuactive = true;
    return;
}



void M_StopMessage(void)
{
    menuactive = messageLastMenuActive;
    messageToPrint = 0;
}



//
// Find string width from hu_font chars
//
int M_StringWidth(char* string)
{
    int             i;
    int             w = 0;
    int             c;
	
    for (i = 0;i < strlen(string);i++)
    {
	c = toupper(string[i]) - HU_FONTSTART;
	if (c < 0 || c >= HU_FONTSIZE)
	    w += 4;
	else
	    w += SHORT (hu_font[c]->width);
    }
		
    return w;
}



//
//      Find string height from hu_font chars
//
int M_StringHeight(char* string)
{
    int             i;
    int             h;
    int             height = SHORT(hu_font[0]->height);
	
    h = height;
    for (i = 0;i < strlen(string);i++)
	if (string[i] == '\n')
	    h += height;
		
    return h;
}


//
//      Write a string using the hu_font
//
void
M_WriteText
( int		x,
  int		y,
  char*		string)
{
    int		w;
    char*	ch;
    int		c;
    int		cx;
    int		cy;
		

    ch = string;
    cx = x;
    cy = y;
	
    while(1)
    {
	c = *ch++;
	if (!c)
	    break;
	if (c == '\n')
	{
	    cx = x;
	    cy += 12;
	    continue;
	}
		
	c = toupper(c) - HU_FONTSTART;
	if (c < 0 || c>= HU_FONTSIZE)
	{
	    cx += 4;
	    continue;
	}
		
	w = SHORT (hu_font[c]->width);
	if (cx+w > SCREENWIDTH)
	    break;
	V_DrawPatchDirect(cx, cy, 0, hu_font[c]);
	cx+=w;
    }
}



//
// CONTROL PANEL
//

//
// M_Responder
//
boolean M_Responder (event_t* ev)
{
    int             ch;
    int             i;
    static  int     joywait = 0;
    static  int     mousewait = 0;
    static  int     mousey = 0;
    static  int     lasty = 0;
    static  int     mousex = 0;
    static  int     lastx = 0;
	
    ch = -1;

    if (Native_Enhanced() && ev->type == ev_mouse)
        return false;
	
    if (ev->type == ev_joystick && joywait < I_GetTime())
    {
	if (ev->data3 == -1)
	{
	    ch = KEY_UPARROW;
	    joywait = I_GetTime() + 5;
	}
	else if (ev->data3 == 1)
	{
	    ch = KEY_DOWNARROW;
	    joywait = I_GetTime() + 5;
	}
		
	if (ev->data2 == -1)
	{
	    ch = KEY_LEFTARROW;
	    joywait = I_GetTime() + 2;
	}
	else if (ev->data2 == 1)
	{
	    ch = KEY_RIGHTARROW;
	    joywait = I_GetTime() + 2;
	}
		
	if (ev->data1&1)
	{
	    ch = KEY_ENTER;
	    joywait = I_GetTime() + 5;
	}
	if (ev->data1&2)
	{
	    ch = KEY_BACKSPACE;
	    joywait = I_GetTime() + 5;
	}
    }
    else
    {
	if (ev->type == ev_mouse && mousewait < I_GetTime())
	{
	    mousey += ev->data3;
	    if (mousey < lasty-30)
	    {
		ch = KEY_DOWNARROW;
		mousewait = I_GetTime() + 5;
		mousey = lasty -= 30;
	    }
	    else if (mousey > lasty+30)
	    {
		ch = KEY_UPARROW;
		mousewait = I_GetTime() + 5;
		mousey = lasty += 30;
	    }
		
	    mousex += ev->data2;
	    if (mousex < lastx-30)
	    {
		ch = KEY_LEFTARROW;
		mousewait = I_GetTime() + 5;
		mousex = lastx -= 30;
	    }
	    else if (mousex > lastx+30)
	    {
		ch = KEY_RIGHTARROW;
		mousewait = I_GetTime() + 5;
		mousex = lastx += 30;
	    }
		
	    if (ev->data1&1)
	    {
		ch = KEY_ENTER;
		mousewait = I_GetTime() + 15;
	    }
			
	    if (ev->data1&2)
	    {
		ch = KEY_BACKSPACE;
		mousewait = I_GetTime() + 15;
	    }
	}
	else
	    if (ev->type == ev_keydown)
	    {
		ch = ev->data1;
	    }
    }
    
    if (ch == -1)
	return false;

    
    // Save Game string input
    if (saveStringEnter)
    {
	switch(ch)
	{
	  case KEY_BACKSPACE:
	    saveNameFresh = false;
	    if (saveCharIndex > 0)
	    {
		saveCharIndex--;
		savegamestrings[saveSlot][saveCharIndex] = 0;
	    }
	    break;
				
	  case KEY_ESCAPE:
	    saveStringEnter = 0;
	    strcpy(&savegamestrings[saveSlot][0],saveOldString);
	    break;
				
	  case KEY_ENTER:
	    saveStringEnter = 0;
	    M_DoSave(saveSlot);
	    break;
				
	  default:
	    ch = toupper(ch);
	    if (ch != 32)
		if (ch-HU_FONTSTART < 0 || ch-HU_FONTSTART >= HU_FONTSIZE)
		    break;
	    if (saveNameFresh && ch >= 32 && ch <= 127)
	    {
		saveCharIndex = 0;
		savegamestrings[saveSlot][0] = 0;
	    }
	    if (ch >= 32 && ch <= 127)
		saveNameFresh = false;
	    if (ch >= 32 && ch <= 127 &&
		saveCharIndex < SAVESTRINGSIZE-1 &&
		M_StringWidth(savegamestrings[saveSlot]) <
		(SAVESTRINGSIZE-2)*8)
	    {
		savegamestrings[saveSlot][saveCharIndex++] = ch;
		savegamestrings[saveSlot][saveCharIndex] = 0;
	    }
	    break;
	}
	return true;
    }
    
    // Take care of any messages that need input. The native panel shows the
    // answers as rows: arrows move between them and Enter takes the one shown.
    if (messageToPrint)
    {
	if (messageNeedsInput && (ch == KEY_UPARROW || ch == KEY_DOWNARROW))
	{
	    messageChoice ^= 1;
	    S_StartSound(NULL,sfx_pstop);
	    return true;
	}
	if (messageNeedsInput && ch == KEY_ENTER)
	    ch = messageChoice ? 'n' : 'y';
	if (messageNeedsInput == true &&
	    !(ch == ' ' || ch == 'n' || ch == 'y' || ch == KEY_ESCAPE))
	    return false;
		
	menuactive = messageLastMenuActive;
	messageToPrint = 0;
	menuEnterSound = sfx_swtchx;
	if (messageRoutine)
	    messageRoutine(ch);
			
	menuactive = false;
	S_StartSound(NULL,menuEnterSound);
	return true;
    }
	
    if (devparm && ch == KEY_F1)
    {
	G_ScreenShot ();
	return true;
    }
		
    
    // F-Keys
    if (!menuactive)
	switch(ch)
	{
	  case KEY_MINUS:         // Screen size down
	    if (automapactive || chat_on)
		return false;
	    M_SizeDisplay(0);
	    S_StartSound(NULL,sfx_stnmov);
	    return true;
				
	  case KEY_EQUALS:        // Screen size up
	    if (automapactive || chat_on)
		return false;
	    M_SizeDisplay(1);
	    S_StartSound(NULL,sfx_stnmov);
	    return true;
				
	  case KEY_F1:            // Help key
	    M_StartControlPanel ();

	    if ( gamemode == retail )
	      currentMenu = &ReadDef2;
	    else
	      currentMenu = &ReadDef1;
	    
	    itemOn = 0;
	    S_StartSound(NULL,sfx_swtchn);
	    return true;
				
	  case KEY_F2:            // Save
	    M_StartControlPanel();
	    S_StartSound(NULL,sfx_swtchn);
	    M_SaveGame(0);
	    return true;
				
	  case KEY_F3:            // Load
	    M_StartControlPanel();
	    S_StartSound(NULL,sfx_swtchn);
	    M_LoadGame(0);
	    return true;
				
	  case KEY_F4:            // Sound Volume
	    M_StartControlPanel ();
	    currentMenu = &SoundDef;
	    itemOn = music_vol;
	    S_StartSound(NULL,sfx_swtchn);
	    return true;
				
	  case KEY_F5:            // Detail toggle
	    M_ChangeDetail(0);
	    S_StartSound(NULL,sfx_swtchn);
	    return true;
				
	  case KEY_F6:            // Quicksave
	    S_StartSound(NULL,sfx_swtchn);
	    M_QuickSave();
	    return true;
				
	  case KEY_F7:            // End game
	    S_StartSound(NULL,sfx_swtchn);
	    M_EndGame(0);
	    return true;
				
	  case KEY_F8:            // Toggle messages
	    M_ChangeMessages(0);
	    S_StartSound(NULL,sfx_swtchn);
	    return true;
				
	  case KEY_F9:            // Quickload
	    S_StartSound(NULL,sfx_swtchn);
	    M_QuickLoad();
	    return true;
				
	  case KEY_F10:           // Quit DOOM
	    S_StartSound(NULL,sfx_swtchn);
	    M_QuitDOOM(0);
	    return true;
				
	  case KEY_F11:           // gamma toggle
	    usegamma++;
	    if (usegamma > 4)
		usegamma = 0;
	    players[consoleplayer].message = gammamsg[usegamma];
	    I_SetPalette (W_CacheLumpName ("PLAYPAL",PU_CACHE));
	    return true;
				
	}

    
    // Pop-up menu?
    if (!menuactive)
    {
	if (ch == KEY_ESCAPE)
	{
	    M_StartControlPanel ();
	    S_StartSound(NULL,sfx_swtchn);
	    return true;
	}
	return false;
    }

    
    // Keys usable within menu
    switch (ch)
    {
      case KEY_DOWNARROW:
	do
	{
	    if (itemOn+1 > currentMenu->numitems-1)
		itemOn = 0;
	    else itemOn++;
	    S_StartSound(NULL,sfx_pstop);
	} while(currentMenu->menuitems[itemOn].status==-1);
	return true;
		
      case KEY_UPARROW:
	do
	{
	    if (!itemOn)
		itemOn = currentMenu->numitems-1;
	    else itemOn--;
	    S_StartSound(NULL,sfx_pstop);
	} while(currentMenu->menuitems[itemOn].status==-1);
	return true;

      case KEY_LEFTARROW:
	if (currentMenu->menuitems[itemOn].routine &&
	    currentMenu->menuitems[itemOn].status == 2)
	{
	    S_StartSound(NULL,sfx_stnmov);
	    currentMenu->menuitems[itemOn].routine(0);
	}
	return true;
		
      case KEY_RIGHTARROW:
	if (currentMenu->menuitems[itemOn].routine &&
	    currentMenu->menuitems[itemOn].status == 2)
	{
	    S_StartSound(NULL,sfx_stnmov);
	    currentMenu->menuitems[itemOn].routine(1);
	}
	return true;

      case KEY_ENTER:
	if (currentMenu->menuitems[itemOn].routine &&
	    currentMenu->menuitems[itemOn].status)
	{
	    currentMenu->lastOn = itemOn;
	    if (currentMenu->menuitems[itemOn].status == 2)
	    {
		currentMenu->menuitems[itemOn].routine(1);      // right arrow
		S_StartSound(NULL,sfx_stnmov);
	    }
	    else
	    {
		menuEnterSound = sfx_pistol;
		currentMenu->menuitems[itemOn].routine(itemOn);
		S_StartSound(NULL,menuEnterSound);
	    }
	}
	return true;
		
      case KEY_ESCAPE:
	currentMenu->lastOn = itemOn;
	// Back steps up one page; only the top level leaves the menu.
	if (currentMenu->prevMenu)
	{
	    currentMenu = currentMenu->prevMenu;
	    itemOn = currentMenu->lastOn;
	    S_StartSound(NULL,sfx_swtchn);
	    return true;
	}
	M_ClearMenus ();
	S_StartSound(NULL,sfx_swtchx);
	return true;
		
      case KEY_BACKSPACE:
	currentMenu->lastOn = itemOn;
	if (currentMenu->prevMenu)
	{
	    currentMenu = currentMenu->prevMenu;
	    itemOn = currentMenu->lastOn;
	    S_StartSound(NULL,sfx_swtchn);
	}
	return true;
	
      default:
	for (i = itemOn+1;i < currentMenu->numitems;i++)
	    if (currentMenu->menuitems[i].alphaKey == ch)
	    {
		itemOn = i;
		S_StartSound(NULL,sfx_pstop);
		return true;
	    }
	for (i = 0;i <= itemOn;i++)
	    if (currentMenu->menuitems[i].alphaKey == ch)
	    {
		itemOn = i;
		S_StartSound(NULL,sfx_pstop);
		return true;
	    }
	break;
	
    }

    return false;
}



//
// M_StartControlPanel
//
void M_StartControlPanel (void)
{
    // intro might call this repeatedly
    if (menuactive)
	return;
    
    menuactive = 1;
    currentMenu = &MainDef;         // JDC
    // In a level the menu is the pause menu: it opens on Resume, and saving
    // and ending the game are offered only there.
    MainMenu[resume].status = Native_Enhanced() && usergame && gamestate==GS_LEVEL && !demoplayback ? 1 : -1;
    if(Native_Enhanced()) MainMenu[quicksave].status = MainMenu[savegame].status = MainMenu[endgamemain].status = MainMenu[resume].status;
    if(netgame) MainMenu[endgamemain].status = -1;
    itemOn = MainMenu[resume].status > 0 ? resume : currentMenu->lastOn;   // JDC
    if (MainMenu[itemOn].status < 0)
	itemOn = newgame;
}


//
// M_Drawer
// Called after the view has been rendered,
// but before it has been blitted.
//
// Native presentation reads the existing menu model; commands still use M_Responder.
int Native_MenuTitle(char *title, int capacity)
{
    const char *name=Native_GameTitle();
    if(messageToPrint) {
        // The answers are rows, so the "press y" line is left out.
        snprintf(title,capacity,"%s",messageString);
        char *prompt=messageNeedsInput?strstr(title,"\n\n"):NULL;
        while(prompt && strstr(prompt+2,"\n\n")) prompt=strstr(prompt+2,"\n\n");
        if(prompt && strstr(prompt,"press")) *prompt=0;
        return messageNeedsInput?2:0;
    }
    if(!menuactive && paused) {
        const char *use=Native_PadButton(NATIVE_BUTTON_SOUTH),*pause=Native_PadButton(NATIVE_BUTTON_PAUSE),*menu=Native_PadButton(NATIVE_BUTTON_MENU);
        if(!use) snprintf(title,capacity,"Paused\nEscape opens the menu");
        else if(pause) snprintf(title,capacity,"Paused\n%s or %s resumes; %s opens the menu",use,pause,menu);
        else snprintf(title,capacity,"Paused\n%s resumes; %s opens the menu",use,menu);
        return 0;
    }
    if(!menuactive) { if(capacity) title[0]=0; return -1; }
    if(currentMenu==&MainDef && MainMenu[resume].status>0) name="Paused";
    if(currentMenu==&EpiDef) name="Choose Episode";
    else if(currentMenu==&NewDef) name="Choose Difficulty";
    else if(currentMenu==&OptionsDef) name="Settings";
    else if(currentMenu==&VideoDef) name="Video";
    else if(currentMenu==&ControlsDef) name="Controls";
    else if(currentMenu==&ControllerDef) {
        // The connected controller's own button names.
        const char *pad=Native_PadButton(NATIVE_BUTTON_NAME);
        if(!pad) { snprintf(title,capacity,"Controller\nNo controller connected. Connect a DualSense, DualShock, Xbox or other game controller with Bluetooth or USB."); return currentMenu->numitems; }
        const char *touch=Native_PadButton(NATIVE_BUTTON_TOUCHPAD),*pause=Native_PadButton(NATIVE_BUTTON_PAUSE);
        int n=snprintf(title,capacity,"Controller\n%s\nFire: %s   Use: %s   Run: %s   Strafe: hold %s\nWeapons: %s, %s, d-pad%s%s\nMenu: %s",pad,
            Native_PadButton(NATIVE_BUTTON_RT),Native_PadButton(NATIVE_BUTTON_SOUTH),Native_PadButton(NATIVE_BUTTON_WEST),Native_PadButton(NATIVE_BUTTON_LT),
            Native_PadButton(NATIVE_BUTTON_LB),Native_PadButton(NATIVE_BUTTON_RB),touch?"   Map: ":"",touch?touch:"",Native_PadButton(NATIVE_BUTTON_MENU));
        if(pause && n>0 && n<capacity) snprintf(title+n,capacity-n,"   Pause: %s",pause);
        return currentMenu->numitems;
    }
    else if(currentMenu==&SoundDef) name="Audio";
    else if(currentMenu==&LoadDef) name="Load Game";
    else if(currentMenu==&SaveDef) name=saveStringEnter?"Name Your Save":"Save Game";
    else if(currentMenu==&ReadDef1 || currentMenu==&ReadDef2)
        name="Controls\nMove: W A S D or arrow keys\nTurn: mouse or left/right arrows\nFire: left click or Control\nUse: Space or Return\nRun: Shift\nWeapons: number keys\nMap: Tab   Menu: Escape\nFullscreen: Command-F   Status bar: Command-H\nQuit: Command-Q\nController buttons: Settings, Controller";
    snprintf(title,capacity,"%s",name);
    return currentMenu->numitems;
}
int Native_MenuSelection(void) { return messageToPrint?messageChoice:itemOn; }
void Native_MenuSelect(int index)
{
    if(messageToPrint && messageNeedsInput && (index==0 || index==1)) messageChoice=index;
    if(menuactive && !messageToPrint && index>=0 && index<currentMenu->numitems && currentMenu->menuitems[index].status>0) itemOn=index;
}
int Native_MenuRow(int index, char *label, int capacity)
{
    if(messageToPrint) {
        if(!messageNeedsInput || index<0 || index>1) return -1;
        snprintf(label,capacity,"%s",index?"Cancel":messageRoutine==M_QuitResponse?"Quit":"Yes");
        return 1;
    }
    if(!menuactive || index<0 || index>=currentMenu->numitems) return -1;
    menuitem_t *item=&currentMenu->menuitems[index];
    if(item->status<0) return -1;
    const char *text=item->name;
    if(currentMenu==&MainDef && index==resume) text="Resume";
    if(currentMenu==&MainDef && index==quicksave) text="Quick Save";
    if(currentMenu==&MainDef && index==endgamemain) text="End Game";
    if(currentMenu==&OptionsDef && index==videoopt) text="Video";
    if(currentMenu==&OptionsDef && index==controlsopt) text="Controls";
    if(currentMenu==&OptionsDef && index==controlleropt) text="Controller";
    if(currentMenu==&VideoDef) {
        static const char *names[]={"Fullscreen","Status Bar HUD","Messages","Skip Transitions"};
        int on[]={Native_Fullscreen(),Native_StatusBar(),showMessages,Native_SkipTransitions()};
        snprintf(label,capacity,"%s    %s",names[index],on[index]?"On":"Off");
        return item->status;
    }
    if(currentMenu==&ControlsDef) {
        if(index==ctl_mouse) snprintf(label,capacity,"Mouse Sensitivity    %d",mouseSensitivity);
        else snprintf(label,capacity,"Controls Reference…");
        return item->status;
    }
    if(currentMenu==&ControllerDef) {
        // DualSense triggers resist; others may vibrate in the triggers.
        // A row the connected controller cannot use is shown dimmed.
        int kind=Native_PadKind(),status=item->status;
        switch(index) {
        case pad_haptics: snprintf(label,capacity,"Vibration    %s",Native_PadValue(NATIVE_PAD_HAPTICS)?"On":"Off"); if(kind && !Native_PadVibration()) status=0; break;
        case pad_hapticstrength: snprintf(label,capacity,"Vibration Strength    %d",Native_PadValue(NATIVE_PAD_HAPTIC_STRENGTH)); if(kind && !Native_PadVibration()) status=0; break;
        case pad_triggers: snprintf(label,capacity,"%s    %s",kind==1?"Adaptive Triggers":"Trigger Effects",Native_PadValue(NATIVE_PAD_TRIGGERS)?"On":"Off"); if(kind && !Native_PadTriggerEffects()) status=0; break;
        case pad_triggerstrength: snprintf(label,capacity,"%s    %d",kind==1?"Trigger Resistance":"Trigger Strength",Native_PadValue(NATIVE_PAD_TRIGGER_STRENGTH)); if(kind && !Native_PadTriggerEffects()) status=0; break;
        case pad_deadzone: snprintf(label,capacity,"Stick Deadzone    %d",Native_PadValue(NATIVE_PAD_DEADZONE)); break;
        default: snprintf(label,capacity,"Turn Sensitivity    %d",Native_PadValue(NATIVE_PAD_TURN)); break;
        }
        return status;
    }
    static const struct { const char *patch,*title; } names[]={
        {"M_NGAME","New Game"},{"M_OPTION","Settings"},{"M_LOADG","Load Game"},{"M_SAVEG","Save Game"},
        {"M_RDTHIS","Controls"},{"M_QUITG","Quit"},
        {"M_EPI1","Knee-Deep in the Dead"},{"M_EPI2","The Shores of Hell"},{"M_EPI3","Inferno"},{"M_EPI4","Thy Flesh Consumed"},
        {"M_JKILL","I'm Too Young to Die"},{"M_ROUGH","Hey, Not Too Rough"},{"M_HURT","Hurt Me Plenty"},{"M_ULTRA","Ultra-Violence"},{"M_NMARE","Nightmare!"},
        {"M_ENDGAM","End Game"},{"M_MESSG","Messages"},{"M_DETAIL","Detail"},{"M_SCRNSZ","View Size"},{"M_MSENS","Mouse Sensitivity"},{"M_SVOL","Audio"},
        {"M_SFXVOL","Sound Effects"},{"M_MUSVOL","Music"}};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) if(!strcmp(item->name,names[i].patch)) { text=names[i].title; break; }
    if(currentMenu==&LoadDef || currentMenu==&SaveDef) {
        snprintf(label,capacity,"%d. %s%s%s",index+1,savegamestrings[index][0]?savegamestrings[index]:"Empty Slot",index==Native_LatestSave()?" (Latest)":"",saveStringEnter && index==saveSlot?"_":"");
    } else if(currentMenu==&SoundDef) {
        snprintf(label,capacity,"Music    %s",Native_MusicEnabled()?"On":"Off");
    } else if(currentMenu==&OptionsDef && index==mousesens) snprintf(label,capacity,"%s    %d",text,mouseSensitivity);
    else if(currentMenu==&OptionsDef && index==messages) snprintf(label,capacity,"%s    %s",text,showMessages?"On":"Off");
    else snprintf(label,capacity,"%s",*text?text:"Continue");
    return item->status;
}

void M_Drawer (void)
{
    if(Native_Enhanced()) { inhelpscreens=false; return; }
    static short	x;
    static short	y;
    short		i;
    short		max;
    char		string[40];
    int			start;

    inhelpscreens = false;

    
    // Horiz. & Vertically center string and print it.
    if (messageToPrint)
    {
	start = 0;
	y = 100 - M_StringHeight(messageString)/2;
	while(*(messageString+start))
	{
	    for (i = 0;i < strlen(messageString+start);i++)
		if (*(messageString+start+i) == '\n')
		{
		    memset(string,0,40);
		    strncpy(string,messageString+start,i);
		    start += i+1;
		    break;
		}
				
	    if (i == strlen(messageString+start))
	    {
		strcpy(string,messageString+start);
		start += i;
	    }
				
	    x = 160 - M_StringWidth(string)/2;
	    M_WriteText(x,y,string);
	    y += SHORT(hu_font[0]->height);
	}
	return;
    }

    if (!menuactive)
	return;

    if (currentMenu->routine)
	currentMenu->routine();         // call Draw routine
    
    // DRAW MENU
    x = currentMenu->x;
    y = currentMenu->y;
    max = currentMenu->numitems;

    for (i=0;i<max;i++)
    {
	if (currentMenu->menuitems[i].name[0])
	    V_DrawPatchDirect (x,y,0,
			       W_CacheLumpName(currentMenu->menuitems[i].name ,PU_CACHE));
	y += LINEHEIGHT;
    }

    
    // DRAW SKULL
    V_DrawPatchDirect(x + SKULLXOFF,currentMenu->y - 5 + itemOn*LINEHEIGHT, 0,
		      W_CacheLumpName(skullName[whichSkull],PU_CACHE));

}


//
// M_ClearMenus
//
void M_ClearMenus (void)
{
    menuactive = 0;
    // if (!netgame && usergame && paused)
    //       sendpause = true;
}




//
// M_SetupNextMenu
//
void M_SetupNextMenu(menu_t *menudef)
{
    currentMenu = menudef;
    itemOn = currentMenu->lastOn;
}


//
// M_Ticker
//
void M_Ticker (void)
{
    if (--skullAnimCounter <= 0)
    {
	whichSkull ^= 1;
	skullAnimCounter = 8;
    }
}


//
// M_Init
//
void M_Init (void)
{
    if(Native_Enhanced()) {
        OptionsMenu[detail].status=-1;
        OptionsMenu[scrnsize].status=-1;
    }
    // Resume and Quick Save are native panel rows; the original layout keeps
    // New Game on top.
    if(!Native_Enhanced()) MainDef.y-=2*LINEHEIGHT;
    MainDef.lastOn=newgame;
    if(Native_Enhanced()) {
        // Messages and mouse sensitivity live in the Video and Controls pages,
        // End Game in the pause menu, the controls reference in Controls (one
        // page; the second, identical one is skipped).
        OptionsMenu[messages].status=-1; OptionsMenu[mousesens].status=-1;
        OptionsMenu[endgame].status=-1; OptionsDef.lastOn=soundvol;
        MainMenu[readthis].status=-1;
        ReadDef1.prevMenu=&ControlsDef; ReadMenu1[0].routine=M_FinishReadThis;
    } else {
        OptionsMenu[videoopt].status=-1; OptionsMenu[controlsopt].status=-1; OptionsMenu[controlleropt].status=-1;
    }
    currentMenu = &MainDef;
    menuactive = 0;
    itemOn = currentMenu->lastOn;
    whichSkull = 0;
    skullAnimCounter = 10;
    screenSize = screenblocks - 3;
    messageToPrint = 0;
    messageString = NULL;
    messageLastMenuActive = menuactive;
    quickSaveSlot = -1;

    // Here we could catch other version dependencies,
    //  like HELP1/2, and four episodes.

  
    switch ( gamemode )
    {
      case commercial:
	// This is used because DOOM 2 had only one HELP
        //  page. I use CREDIT as second page now, but
	//  kept this hack for educational purposes.
	MainMenu[readthis] = MainMenu[quitdoom];
	MainDef.numitems--;
	MainDef.y += 8;
	NewDef.prevMenu = &MainDef;
	ReadDef1.routine = M_DrawReadThis1;
	ReadDef1.x = 330;
	ReadDef1.y = 165;
	ReadMenu1[0].routine = M_FinishReadThis;
	break;
      case shareware:
	// Episode 2 and 3 are handled,
	//  branching to an ad screen.
      case registered:
	// We need to remove the fourth episode.
	EpiDef.numitems--;
	break;
      case retail:
	// We are fine.
      default:
	break;
    }
    
}
