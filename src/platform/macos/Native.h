// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
const char *Native_IWADPath(void);
int Native_GameMode(void);
int Native_GameMission(void);
const char *Native_GameTitle(void);
const char *Native_ProfileConfig(void);
const char *Native_SavePath(int slot);
int Native_LatestSave(void);
int Native_SaveExists(int slot);
int Native_SaveFileValid(const char *path);
int Native_WriteSave(int slot, const void *data, unsigned length);
int Native_ValidLevel(int episode, int map);
void Native_ToggleHUD(void);
extern int native_exitstatus;
// In-game menu equivalents of the former menu bar items.
void Native_SetStatusBar(int on);
int Native_Fullscreen(void);
// Skip Transitions (on by default): no screen melt, and a finished level goes
// straight on past the tally.
int Native_SkipTransitions(void);
void Native_SetSkipTransitions(int on);
// The connected controller: 0 none, 1 DualSense, 2 any other. Its name and
// the names printed on its buttons (NULL when it has no such button), and
// whether it can vibrate and has trigger effects.
enum { NATIVE_BUTTON_NAME, NATIVE_BUTTON_SOUTH, NATIVE_BUTTON_EAST, NATIVE_BUTTON_WEST, NATIVE_BUTTON_NORTH, NATIVE_BUTTON_LB, NATIVE_BUTTON_RB,
       NATIVE_BUTTON_LT, NATIVE_BUTTON_RT, NATIVE_BUTTON_MENU, NATIVE_BUTTON_PAUSE, NATIVE_BUTTON_TOUCHPAD };
int Native_PadKind(void);
const char *Native_PadButton(int button);
int Native_PadVibration(void);
int Native_PadTriggerEffects(void);
void Native_ToggleFullscreen(void);
enum { NATIVE_PAD_DEADZONE, NATIVE_PAD_TURN, NATIVE_PAD_HAPTICS, NATIVE_PAD_HAPTIC_STRENGTH, NATIVE_PAD_TRIGGERS, NATIVE_PAD_TRIGGER_STRENGTH };
int Native_PadValue(int setting);
void Native_PadAdjust(int setting, int direction);
int Native_MusicEnabled(void);
void Native_SetMusicEnabled(int on);
enum NativeFeedbackKind { NATIVE_WEAPON_FIRE=1, NATIVE_DAMAGE=2, NATIVE_EXPLOSION=3, NATIVE_PICKUP=4, NATIVE_IMPACT=5, NATIVE_DOOR=6, NATIVE_LIFT=7, NATIVE_SWITCH=8 };

const char *Native_ConfigPath(void);
int Native_RegisteredIWAD(void);
void Native_Pump(void);
void Native_Present(const uint8_t *pixels);
void Native_SetPalette(const uint8_t *palette);
void Native_Shutdown(void);
void Native_QuitFade(void);
int Native_Time(void);
int Native_IsActive(void);
int Native_Enhanced(void);
int Native_StatusBar(void);
void Native_FeedbackPlayer(int player, int kind, int detail);
void Native_FeedbackSource(int player, int32_t x, int32_t y);
int Native_ControllerLastTurn(void);
int Native_ControllerPendingTurn(void);
void Native_BeginFrame(void);
int Native_ScheduleFrames(void (*frame)(void));
double Native_PresentationTics(void);
double Native_ClockTics(void);
void Native_CommandBoundary(int commandTic, int clockTic);
void Native_RenderReset(void);
void Native_RenderDiscontinuity(void *actor);
void Native_RenderDespawn(void *actor);
void Native_RenderSnapshot(void);
void Native_RenderPrepare(void);
uint16_t *Native_LightingBegin(unsigned width, unsigned height);
const uint16_t *Native_LightingPixels(unsigned width, unsigned height);
void *Native_RenderMobj(void *actor);
void *Native_RenderSector(void *sector);
void Native_RenderCamera(void *player, int32_t *x, int32_t *y, int32_t *z, uint32_t *angle);
void Native_RenderPSprite(void *player, int index, int32_t *x, int32_t *y);
double Native_RenderLevelTime(void);
// Cosmetic gore: reads game state, never writes it or calls the gameplay RNG.
typedef struct { int32_t x, y, z, radius, stretch; int color, chunk; } native_particle_t;
void Native_GoreReset(void);
void Native_GoreShot(void *target, int32_t x, int32_t y, int32_t z, int32_t dx, int32_t dy, int32_t slope, int damage, int corpse);
void Native_GoreHit(void *target, void *source, void *inflictor, int damage);
// Presentation-only death physics: vertical lift of a fresh corpse.
void Native_BodyHit(void *target, void *inflictor, void *source, int damage);
int32_t Native_BodyLift(const void *actor);
void Native_BodyFlinch(const void *actor, int32_t *x, int32_t *y);
void Native_BodyForget(const void *actor);
void Native_GoreCrush(void *thing);
void Native_GoreDespawn(void *thing);
void Native_GoreTic(void);
void Native_GoreSound(int kind, double x, double y, double z, double strength);
void Native_GoreFrame(void);
void Native_GoreSprites(void *sector, void (*project)(const native_particle_t *));
void Native_GorePlanes(void);
const void *Native_GoreWallSide(int line, int side);
void Native_GoreWallColumn(const void *side, int32_t u, int yl, int yh, int x, int32_t iscale,
                           const void *colormap, const void *colormapNext, unsigned lightweight);
uint16_t *R_NativeLightPixels(void);
int Native_RenderSectorIndex(const void *sector);
void Native_ViewmodelBegin(void *player);
void Native_ViewmodelPatch(int lump, int flip, int32_t sx, int32_t sy, const uint8_t *map);
const uint16_t *Native_ViewmodelPixels(void);
const uint8_t *Native_ViewmodelFuzzMap(void);
#define NATIVE_MAX_WIDTH 4096
#define NATIVE_MAX_HEIGHT 2560
extern int native_render_width, native_render_height;
void Native_RenderSize(int blocks);
uint8_t *Native_RenderPixels(void);
void Native_MetricsRenderBegin(void);
void Native_MetricsRenderEnd(void);
int Native_MenuTitle(char *title, int capacity);
int Native_MenuRow(int index, char *label, int capacity);
int Native_MenuSelection(void);
void Native_MenuSelect(int index);

void Native_Error(const char *message);
void Native_ControllerPoll(void);
void Native_ControllerReset(void);
void Native_Feedback(int kind, int detail);
void Native_FeedbackUpdate(void);
// Flesh-impact sound for a hit landing in a bleeding monster (audio only).
void Native_FleshImpact(void *target, int damage);
void Native_AudioSuspend(int suspended);
void Native_AudioService(void);
#ifdef __cplusplus
}
#endif
